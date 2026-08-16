/**
 * Sign-in and authenticated API access.
 *
 * MSAL authorization-code flow with PKCE. Tokens live in sessionStorage, so
 * closing the tab ends the session -- a reasonable default for something that
 * administers door access on machines that may be shared.
 *
 * ROLES ARE NOT A UI CONCERN. The role in the token decides what this app SHOWS,
 * but every route is enforced again server-side against the same token. Hiding a
 * button is a courtesy, not a control: anyone can open dev tools and call the API
 * directly. If the two disagree, the server wins -- and the server is the one
 * that matters.
 */
import { PublicClientApplication, InteractionRequiredAuthError } from '@azure/msal-browser';
import { TENANT_ID, CLIENT_ID, API_BASE, SCOPE } from './config';

const msal = new PublicClientApplication({
  auth: {
    clientId: CLIENT_ID,
    authority: `https://login.microsoftonline.com/${TENANT_ID}`,
    redirectUri: window.location.origin,
  },
  cache: { cacheLocation: 'sessionStorage', storeAuthStateInCookie: false },
});

let account = null;

export async function initAuth() {
  await msal.initialize();
  const result = await msal.handleRedirectPromise();
  account = result?.account ?? msal.getAllAccounts()[0] ?? null;
  return account;
}

export const signIn = () =>
  msal.loginRedirect({ scopes: [SCOPE], prompt: 'select_account' });
export const signOut = () => msal.logoutRedirect({ account });
export const currentAccount = () => account;

const RANK = { Viewer: 1, Operator: 2, Admin: 3 };

/** Roles come from Entra app-role assignments on the security groups. */
export function effectiveRole() {
  const claims = account?.idTokenClaims ?? {};
  const roles = (Array.isArray(claims.roles) ? claims.roles : [])
    .filter((r) => r in RANK)
    .sort((a, b) => RANK[b] - RANK[a]);
  return roles[0] ?? null;
}

export function atLeast(role) {
  const mine = effectiveRole();
  return !!mine && RANK[mine] >= RANK[role];
}

async function getToken() {
  if (!account) throw new Error('not signed in');
  const request = { scopes: [SCOPE], account };
  try {
    return (await msal.acquireTokenSilent(request)).accessToken;
  } catch (e) {
    if (e instanceof InteractionRequiredAuthError) {
      await msal.acquireTokenRedirect(request);
      return null;                       // navigating away
    }
    throw e;
  }
}

/**
 * Errors carry `.status` so callers can distinguish 403 (you lack the role;
 * signing in again will not help) from 401 (session went stale; it will).
 * Conflating them produces a login loop that can never succeed.
 */
export async function api(path, options = {}) {
  const token = await getToken();
  if (!token) return null;

  const res = await fetch(`${API_BASE}${path}`, {
    ...options,
    headers: {
      ...(options.headers ?? {}),
      Authorization: `Bearer ${token}`,
      ...(options.body ? { 'Content-Type': 'application/json' } : {}),
    },
  });

  if (!res.ok) {
    let detail = '';
    try { detail = (await res.json())?.error ?? ''; } catch { /* non-JSON body */ }
    const err = new Error(detail || `HTTP ${res.status}`);
    err.status = res.status;
    throw err;
  }
  return res.status === 204 ? null : res.json();
}
