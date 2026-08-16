/**
 * POST /api/GetRoles — the SWA rolesSource hook.
 *
 * Static Web Apps calls this once, immediately after a successful Entra sign-in,
 * and whatever it returns becomes the user's roles for the session.
 *
 * WHY THIS EXISTS AT ALL
 * SWA's built-in roles are only `anonymous` and `authenticated`. The Entra app
 * roles assigned to our security groups arrive as a `roles` claim inside the
 * token, but SWA does not surface those automatically — without this hook every
 * signed-in user would look identical to the API, and role separation would be
 * decorative.
 *
 * So: read the app roles out of the claims, hand them back as SWA roles. The
 * group-to-role mapping lives in Entra (app role assignments), not in code — the
 * point being that "who is an admin?" is answered by looking at a security group,
 * not by reading a source file.
 *
 * A user in several groups holds several roles; all are returned and the API
 * takes the highest.
 */

import { app, HttpRequest, HttpResponseInit, InvocationContext } from '@azure/functions';

interface RolesRequest {
  identityProvider?: string;
  userId?: string;
  userDetails?: string;
  claims?: { typ: string; val: string }[];
}

const KNOWN = new Set(['Viewer', 'Operator', 'Admin']);

export async function getRoles(
  req: HttpRequest,
  ctx: InvocationContext
): Promise<HttpResponseInit> {
  let body: RolesRequest;
  try {
    body = (await req.json()) as RolesRequest;
  } catch {
    return { status: 200, jsonBody: { roles: [] } };
  }

  // Entra emits app roles under the short type "roles"; some token shapes use
  // the full URI form. Accept either rather than depending on which one arrives.
  const roles = (body.claims ?? [])
    .filter(
      (c) =>
        c.typ === 'roles' ||
        c.typ === 'http://schemas.microsoft.com/ws/2008/06/identity/claims/role'
    )
    .map((c) => c.val)
    .filter((v) => KNOWN.has(v));

  const unique = [...new Set(roles)];

  if (unique.length === 0) {
    // Authenticated but in none of the three groups. Returning no roles means
    // the API refuses every admin route with 403 rather than 401 — "you are
    // signed in but not authorised", which is the honest message and stops the
    // UI bouncing the user through a login loop that cannot fix anything.
    ctx.warn(
      `GetRoles: ${body.userDetails ?? body.userId} signed in with no assigned role ` +
        `- add them to a JTC Access Control group`
    );
  } else {
    ctx.log(`GetRoles: ${body.userDetails ?? body.userId} -> ${unique.join(', ')}`);
  }

  return { status: 200, jsonBody: { roles: unique } };
}

app.http('GetRoles', {
  methods: ['POST'],
  // Called by the SWA platform itself, not by a browser. SWA authenticates this
  // call; it is not reachable as a normal route.
  authLevel: 'anonymous',
  route: 'GetRoles',
  handler: getRoles,
});
