/**
 * Authorization for the admin surface — cryptographic, not header-based.
 *
 * WHY THIS IS NOT THE OBVIOUS DESIGN
 * Static Web Apps can forward a signed-in user as an `x-ms-client-principal`
 * header, and trusting that is the usual shortcut. It is only safe while every
 * request arrives THROUGH SWA, which strips client-supplied copies. The Function
 * App is on a public hostname, so anyone who knows the URL can send that header
 * themselves and become an Admin. That is not theoretical: it was demonstrated
 * against this very API with one curl command.
 *
 * So the browser obtains a real Entra access token and sends it as
 * `Authorization: Bearer …`, and this module verifies:
 *
 *   - the SIGNATURE, against Entra's published JWKS for the tenant
 *   - the ISSUER, so a token from another tenant is refused
 *   - the AUDIENCE, so a token minted for a different API is refused
 *   - expiry and not-before
 *
 * A forged header is worthless because there is a signature to check, and a
 * signature cannot be produced without Entra's private key.
 *
 * Roles arrive in the `roles` claim, populated by Entra from the app-role
 * assignments on the security groups — so "who is an admin?" is still answered
 * by looking at a group, not by anything in this code.
 */

import { HttpRequest } from '@azure/functions';

/**
 * Only the claims this module reads. Declared locally rather than imported from
 * jose: importing a TYPE from an ESM-only package into a CommonJS file needs a
 * resolution-mode attribute, which is fiddly and TypeScript-version sensitive.
 * The runtime checks below are what actually enforce correctness anyway.
 */
interface VerifiedClaims {
  oid?: unknown;
  sub?: unknown;
  preferred_username?: unknown;
  upn?: unknown;
  roles?: unknown;
}

export type Role = 'Viewer' | 'Operator' | 'Admin';

/** Ranked, so a check asks for "Operator or better" rather than listing roles. */
const RANK: Record<Role, number> = { Viewer: 1, Operator: 2, Admin: 3 };

const TENANT_ID = process.env.ENTRA_TENANT_ID ?? '';
const CLIENT_ID = process.env.ENTRA_CLIENT_ID ?? '';

// jose v5 is ESM-only and this package emits CommonJS (what the Functions host
// loads), so it is imported dynamically and cached. verifyToken is already
// async, so this costs one promise on first use and nothing after.
let josePromise: Promise<any> | undefined;
const loadJose = (): Promise<any> => (josePromise ??= import('jose'));

// Entra publishes its signing keys here and rotates them periodically. jose
// caches the set and refetches on an unknown key id, so rotation is handled
// without a redeploy — and without us ever holding a key.
let jwks: unknown;
async function getJwks(): Promise<unknown> {
  if (!TENANT_ID) return undefined;
  if (!jwks) {
    const { createRemoteJWKSet } = await loadJose();
    jwks = createRemoteJWKSet(
      new URL(`https://login.microsoftonline.com/${TENANT_ID}/discovery/v2.0/keys`)
    );
  }
  return jwks;
}

const ISSUER = TENANT_ID ? `https://login.microsoftonline.com/${TENANT_ID}/v2.0` : '';

export interface Principal {
  userId: string;
  userDetails: string;   // UPN / preferred_username
  roles: Role[];
  effective?: Role;      // highest held
}

export interface Denied {
  status: number;
  jsonBody: { error: string };
}

function rolesFrom(payload: VerifiedClaims): Role[] {
  const raw = payload.roles;
  const list = Array.isArray(raw) ? raw.map(String) : [];
  const known: Role[] = ['Viewer', 'Operator', 'Admin'];
  return list.filter((r): r is Role => known.includes(r as Role));
}

/**
 * Verify the bearer token. Returns undefined for anything not provably valid —
 * no partial trust, no "looks about right".
 */
export async function verifyToken(req: HttpRequest): Promise<Principal | undefined> {
  if (!CLIENT_ID) return undefined;            // misconfigured: fail closed

  const header = req.headers.get('authorization') ?? '';
  const match = /^Bearer\s+(.+)$/i.exec(header.trim());
  if (!match) return undefined;

  const keys = await getJwks();
  if (!keys) return undefined;                 // no tenant configured: fail closed

  try {
    const { jwtVerify } = await loadJose();
    const { payload } = (await jwtVerify(match[1]!, keys, {
      issuer: ISSUER,
      // Entra v2 tokens carry the bare client id as audience; the api:// form is
      // accepted too so a token requested either way validates.
      audience: [CLIENT_ID, `api://${CLIENT_ID}`],
      clockTolerance: 60,      // a minute of drift, no more
    })) as { payload: VerifiedClaims };

    const roles = rolesFrom(payload).sort((a, b) => RANK[b] - RANK[a]);
    return {
      userId: String(payload.oid ?? payload.sub ?? ''),
      userDetails: String(payload.preferred_username ?? payload.upn ?? ''),
      roles,
      effective: roles[0],
    };
  } catch {
    // Bad signature, wrong issuer or audience, expired, malformed — all the same
    // answer. Distinguishing them would only help someone probing.
    return undefined;
  }
}

/**
 * Require at least `min`.
 *
 * 401 vs 403 is deliberate: 401 means "not signed in" and the UI should send the
 * user to log in; 403 means "signed in but lacking the role", where logging in
 * again cannot help and the UI should say so rather than loop.
 */
export async function requireRole(
  req: HttpRequest,
  min: Role
): Promise<{ principal: Principal } | { denied: Denied }> {
  const p = await verifyToken(req);
  if (!p) {
    return { denied: { status: 401, jsonBody: { error: 'missing or invalid token' } } };
  }
  if (!p.effective || RANK[p.effective] < RANK[min]) {
    return {
      denied: {
        status: 403,
        jsonBody: {
          error: `requires ${min}; you have ${p.effective ?? 'no assigned role'}`,
        },
      },
    };
  }
  return { principal: p };
}

export function isDenied(
  r: { principal: Principal } | { denied: Denied }
): r is { denied: Denied } {
  return 'denied' in r;
}

/**
 * One-line audit string. Every mutating admin action is logged with who did it:
 * an access-control system where changes are anonymous is worth much less than
 * one where "who removed Carl's access on Tuesday" is answerable.
 */
export function actor(p: Principal): string {
  return `${p.userDetails || p.userId}[${p.effective}]`;
}
