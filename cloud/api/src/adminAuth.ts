/**
 * Authorization for the admin surface.
 *
 * Completely separate from device authentication. A device key authenticates a
 * DOOR and permits exactly two things — uploading that door's events and
 * fetching that door's roster. It can never reach anything here. Conversely a
 * signed-in human can never impersonate a door. Keeping the two credential
 * systems disjoint means compromising a controller in a corridor grants nothing
 * on the admin surface, and vice versa.
 *
 * HOW IDENTITY ARRIVES
 * Static Web Apps performs the Entra sign-in and forwards the result to the API
 * as an `x-ms-client-principal` header: base64-encoded JSON naming the user and
 * their roles. SWA strips any client-supplied copy of that header before
 * forwarding, so it cannot be spoofed through the front door.
 *
 * ⚠️ It CAN be spoofed by calling the Function App's own hostname directly,
 * bypassing SWA entirely. That is why the Function App must only be reachable
 * through the linked SWA in production, and why every handler here re-checks the
 * role rather than trusting that a route was reached at all.
 */

import { HttpRequest } from '@azure/functions';

export type Role = 'Viewer' | 'Operator' | 'Admin';

/** Ranked, so a check can ask for "Operator or better" rather than listing roles. */
const RANK: Record<Role, number> = { Viewer: 1, Operator: 2, Admin: 3 };

export interface Principal {
  userId: string;
  userDetails: string;    // the UPN, e.g. someone@awesomewildstuff.com
  roles: Role[];
  /** Highest role held, or undefined if the user has none of ours. */
  effective?: Role;
}

interface SwaPrincipal {
  identityProvider?: string;
  userId?: string;
  userDetails?: string;
  userRoles?: string[];
}

export function getPrincipal(req: HttpRequest): Principal | undefined {
  const header = req.headers.get('x-ms-client-principal');
  if (!header) return undefined;

  let parsed: SwaPrincipal;
  try {
    parsed = JSON.parse(Buffer.from(header, 'base64').toString('utf8'));
  } catch {
    return undefined;
  }

  const known: Role[] = ['Viewer', 'Operator', 'Admin'];
  const roles = (parsed.userRoles ?? []).filter((r): r is Role =>
    known.includes(r as Role)
  );

  // Highest wins. A user in several groups legitimately holds several roles;
  // taking the maximum means promoting someone does not require removing them
  // from the baseline group first.
  const effective = roles.sort((a, b) => RANK[b] - RANK[a])[0];

  return {
    userId: parsed.userId ?? '',
    userDetails: parsed.userDetails ?? '',
    roles,
    effective,
  };
}

export interface Denied {
  status: number;
  jsonBody: { error: string };
}

/**
 * Require at least `min`. Returns the principal, or a ready-to-return response.
 *
 * 401 vs 403 is deliberate: 401 means "you are not signed in" (the UI should send
 * you to log in), 403 means "you are signed in but lack the role" (logging in
 * again will not help, and the UI should say so rather than loop).
 */
export function requireRole(
  req: HttpRequest,
  min: Role
): { principal: Principal } | { denied: Denied } {
  const p = getPrincipal(req);
  if (!p) {
    return { denied: { status: 401, jsonBody: { error: 'not signed in' } } };
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

export function isDenied(r: ReturnType<typeof requireRole>): r is { denied: Denied } {
  return 'denied' in r;
}

/**
 * A one-line audit string for the log.
 *
 * Every mutating admin action gets logged with who did it. An access-control
 * system where changes are anonymous is worth much less than one where they are
 * not — "who removed Carl's access on Tuesday" should be answerable.
 */
export function actor(p: Principal): string {
  return `${p.userDetails || p.userId}[${p.effective}]`;
}
