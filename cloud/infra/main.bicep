/*
  RFID Access - Azure infrastructure.

  Deployed at RESOURCE GROUP scope, which is why this file contains no tenant id,
  no subscription id, and no secrets: `az deployment group create` supplies all
  of that from the deployment context. That is deliberate -- this repo is public,
  and those identifiers have no business in it.

  Deploy:
    az deployment group create \
      --resource-group JTC-prod-rfidaccess-eastus2-rg \
      --template-file main.bicep \
      --parameters @main.parameters.json

  What this creates:
    - Storage account (Table Storage for data, Blob for firmware images)
    - Tables, pre-created so the API never has to create-on-first-write
    - Function App (Linux, Node 24, Flex Consumption) + plan
    - Application Insights + Log Analytics
    - A system-assigned managed identity for the Function App, granted data-plane
      access to storage by RBAC -- so no storage keys are ever stored in app
      settings or handled by a human.
*/

@description('Base name following the JTC-{env}-{app}-{region} convention.')
param baseName string = 'JTC-prod-rfidaccess-eastus2'

@description('Storage account name: 3-24 chars, lowercase alphanumeric, globally unique.')
@minLength(3)
@maxLength(24)
param storageName string = 'jtcprodrfidaccessst'

@description('''
Location for every resource. Defaults to the resource group, which must be in a
region offering Flex Consumption -- eastus does NOT, eastus2 does.

Flex is what allows the deployment package to be read using the app's managed
identity instead of a storage account key, which is what lets the data account
keep allowSharedKeyAccess=false. Without that, a key would exist that unlocks
the entire access-event history.

Everything shares one region deliberately: the sync endpoint touches storage on
every request, so a cross-region hop would add ~15 ms to a path that runs twice
a minute per door, for no benefit.
''')
param location string = resourceGroup().location

@description('''
Entra tenant ID the admin API validates tokens against.

Supplied as a parameter, not hardcoded: this template is in a public repo, and
while a tenant id is not a secret it names exactly which tenant to target. Lives
in main.parameters.json, which is gitignored.
''')
param entraTenantId string

@description('Application (client) ID of the admin app registration. Becomes the expected token audience.')
param entraClientId string

@description('''
Origins allowed to call the API from a browser.

The admin app is a cross-origin caller by design -- it holds an Entra token and
talks to the Function App directly rather than being proxied by Static Web Apps.
Declared here so a new customer deployment gets it from the template rather than
a remembered CLI step.
''')
param allowedOrigins array

@description('Retention for logs and traces. 30 days is ample for fleet debugging.')
param retentionDays int = 30

var tables = [
  'People'
  'Credentials'
  'Groups'
  'Doors'
  'Meta'
  'EventsByPerson'
  'EventsByDoor'
]

// ---------------------------------------------------------------------------
// Storage: Table Storage for all data, Blob for firmware images.
// ---------------------------------------------------------------------------

resource storage 'Microsoft.Storage/storageAccounts@2023-05-01' = {
  name: storageName
  location: location
  sku: {
    // Locally redundant is the right trade here: the device spool is itself a
    // durable buffer, so a brief storage outage costs nothing permanent.
    name: 'Standard_LRS'
  }
  kind: 'StorageV2'
  properties: {
    minimumTlsVersion: 'TLS1_2'
    supportsHttpsTrafficOnly: true
    allowBlobPublicAccess: false
    // Firmware images are fetched by devices using a SAS URL issued at sync
    // time, never by anonymous public read.
    allowSharedKeyAccess: false
    networkAcls: {
      defaultAction: 'Allow'
      bypass: 'AzureServices'
    }
  }
}

resource tableService 'Microsoft.Storage/storageAccounts/tableServices@2023-05-01' = {
  parent: storage
  name: 'default'
}

// Pre-create every table. Creating on first write means every cold start races
// to create the same table and has to handle "already exists" -- avoidable noise
// in the request path.
resource tableResources 'Microsoft.Storage/storageAccounts/tableServices/tables@2023-05-01' = [
  for t in tables: {
    parent: tableService
    name: t
  }
]

resource blobService 'Microsoft.Storage/storageAccounts/blobServices@2023-05-01' = {
  parent: storage
  name: 'default'
}

resource firmwareContainer 'Microsoft.Storage/storageAccounts/blobServices/containers@2023-05-01' = {
  parent: blobService
  name: 'firmware'
  properties: {
    publicAccess: 'None'
  }
}

// Archive target for the retention job: events older than N months are written
// here as JSON and deleted from Table Storage.
resource archiveContainer 'Microsoft.Storage/storageAccounts/blobServices/containers@2023-05-01' = {
  parent: blobService
  name: 'event-archive'
  properties: {
    publicAccess: 'None'
  }
}

// Flex Consumption stores the deployment package here and reads it back using
// the app's managed identity. This is what removes the need for a storage
// account key at deploy time -- the reason for choosing Flex over Y1.
resource deploymentContainer 'Microsoft.Storage/storageAccounts/blobServices/containers@2023-05-01' = {
  parent: blobService
  name: 'deployment'
  properties: {
    publicAccess: 'None'
  }
}

// ---------------------------------------------------------------------------
// Observability. Worth having from day one: the failure this design is most
// exposed to is a door quietly failing to sync, which is invisible without it.
// ---------------------------------------------------------------------------

resource logs 'Microsoft.OperationalInsights/workspaces@2023-09-01' = {
  name: '${baseName}-log'
  location: location
  properties: {
    sku: {
      name: 'PerGB2018'
    }
    retentionInDays: retentionDays
  }
}

resource insights 'Microsoft.Insights/components@2020-02-02' = {
  name: '${baseName}-appi'
  location: location
  kind: 'web'
  properties: {
    Application_Type: 'web'
    WorkspaceResourceId: logs.id
  }
}

// ---------------------------------------------------------------------------
// Function App
// ---------------------------------------------------------------------------

resource plan 'Microsoft.Web/serverfarms@2023-12-01' = {
  name: '${baseName}-plan'
  location: location
  sku: {
    // Flex Consumption. Chosen over Y1 specifically because it can pull its
    // deployment package using the app's managed identity, so the data storage
    // account never needs allowSharedKeyAccess enabled. Also scales to zero and
    // cold-starts faster than Y1.
    name: 'FC1'
    tier: 'FlexConsumption'
  }
  kind: 'functionapp'
  properties: {
    reserved: true // Linux
  }
}

resource functionApp 'Microsoft.Web/sites@2023-12-01' = {
  name: '${baseName}-func'
  location: location
  kind: 'functionapp,linux'
  identity: {
    // System-assigned identity: used both to reach storage at runtime AND to
    // read the deployment package at start-up.
    type: 'SystemAssigned'
  }
  properties: {
    serverFarmId: plan.id
    httpsOnly: true
    functionAppConfig: {
      deployment: {
        storage: {
          type: 'blobContainer'
          value: '${storage.properties.primaryEndpoints.blob}${deploymentContainer.name}'
          authentication: {
            // No key, no SAS. The Blob Data Contributor assignment below is what
            // lets the app read its own package.
            type: 'SystemAssignedIdentity'
          }
        }
      }
      scaleAndConcurrency: {
        // 20 doors at one request per 30s is a trickle; this ceiling exists to
        // bound cost if something ever misbehaves, not because it is needed.
        maximumInstanceCount: 40
        instanceMemoryMB: 2048
      }
      runtime: {
        // Node 20 reached end-of-life 2026-04-30. 24 runs to April 2029 -- the
        // longest runway available, which matters for unattended infrastructure.
        // On Flex the runtime is declared HERE, not via linuxFxVersion, and
        // FUNCTIONS_WORKER_RUNTIME / FUNCTIONS_EXTENSION_VERSION must NOT be set
        // as app settings; doing so conflicts with this block.
        name: 'node'
        version: '24'
      }
    }
    siteConfig: {
      minTlsVersion: '1.2'
      ftpsState: 'Disabled'
      http20Enabled: true
      cors: {
        allowedOrigins: allowedOrigins
        // The API authenticates with a bearer token, never a cookie, so there is
        // no reason to allow credentialed cross-origin requests.
        supportCredentials: false
      }
      appSettings: [
        {
          name: 'APPLICATIONINSIGHTS_CONNECTION_STRING'
          value: insights.properties.ConnectionString
        }
        {
          // Identity-based connection: the runtime uses the managed identity
          // above, so no account key is stored anywhere.
          name: 'AzureWebJobsStorage__accountName'
          value: storage.name
        }
        {
          name: 'STORAGE_ACCOUNT_NAME'
          value: storage.name
        }
        {
          name: 'FIRMWARE_CONTAINER'
          value: firmwareContainer.name
        }
        {
          name: 'ARCHIVE_CONTAINER'
          value: archiveContainer.name
        }
        // ⚠️ EVERY app setting must be declared here. siteConfig.appSettings is
        // AUTHORITATIVE: a deployment replaces the whole collection, so anything
        // set out-of-band with `az functionapp config appsettings set` is silently
        // erased on the next `az deployment group create`. That is exactly how
        // these two went missing and every admin request started returning 401.
        {
          name: 'ENTRA_TENANT_ID'
          value: entraTenantId
        }
        {
          name: 'ENTRA_CLIENT_ID'
          value: entraClientId
        }
      ]
    }
  }
}

// ---------------------------------------------------------------------------
// RBAC: let the Function App's identity read and write table + blob data.
// Scoped to this storage account only.
// ---------------------------------------------------------------------------

var tableDataContributor = subscriptionResourceId(
  'Microsoft.Authorization/roleDefinitions',
  '0a9a7e1f-b9d0-4cc4-a60d-0319b160aaa3'
)
var blobDataContributor = subscriptionResourceId(
  'Microsoft.Authorization/roleDefinitions',
  'ba92f5b4-2d11-453d-a403-e96b0029c9fe'
)

resource tableRole 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(storage.id, functionApp.id, 'tableDataContributor')
  scope: storage
  properties: {
    roleDefinitionId: tableDataContributor
    principalId: functionApp.identity.principalId
    principalType: 'ServicePrincipal'
  }
}

resource blobRole 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  name: guid(storage.id, functionApp.id, 'blobDataContributor')
  scope: storage
  properties: {
    roleDefinitionId: blobDataContributor
    principalId: functionApp.identity.principalId
    principalType: 'ServicePrincipal'
  }
}

// ---------------------------------------------------------------------------
// Static Web App: hosts the admin UI.
//
// FREE tier, and static hosting ONLY -- no linked backend, no managed API. The
// browser calls the Function App directly with an Entra bearer token that the
// API verifies cryptographically. That choice is deliberate: SWA's linked-backend
// mode injects an x-ms-client-principal header the API would have to TRUST, and
// that trust only holds while every request arrives through SWA. The Function App
// is on a public hostname, so a forged header would otherwise grant Admin to
// anyone who knows the URL. Verifying a signature removes the assumption
// entirely -- and costs nothing, since linked backends need Standard tier.
//
// Consequence: the SPA is a cross-origin caller, so the Function App needs CORS
// configured for this hostname.
// ---------------------------------------------------------------------------

resource swa 'Microsoft.Web/staticSites@2023-12-01' = {
  name: '${baseName}-swa'
  location: location
  sku: {
    name: 'Free'
    tier: 'Free'
  }
  properties: {
    // No repository is linked: content is pushed with the CLI rather than built
    // from a git integration, so there is no deployment token in any pipeline.
    allowConfigFileUpdates: true
  }
}

output staticWebAppName string = swa.name
output staticWebAppHost string = swa.properties.defaultHostname

output functionAppName string = functionApp.name
output functionAppHost string = functionApp.properties.defaultHostName
output storageAccount string = storage.name
output appInsightsName string = insights.name
