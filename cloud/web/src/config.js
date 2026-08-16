/**
 * Deployment configuration, read from Vite env vars at BUILD time.
 *
 * Only variables prefixed VITE_ are exposed to the browser, which makes "is this
 * shipped to the client?" a visible property of the name rather than something
 * you have to remember. Values live in .env.local (gitignored).
 *
 * There is no client secret here or anywhere in this app. A single-page app
 * cannot hold one -- anything shipped to a browser is readable by whoever opens
 * dev tools. Sign-in uses authorization-code flow with PKCE, which exists
 * precisely so a public client can authenticate without a secret.
 */
const required = (name) => {
  const v = import.meta.env[name];
  if (!v) throw new Error(`${name} is not set — copy .env.example to .env.local`);
  return v;
};

export const TENANT_ID = required('VITE_TENANT_ID');
export const CLIENT_ID = required('VITE_CLIENT_ID');
export const API_BASE = required('VITE_API_BASE');
export const SCOPE = `api://${CLIENT_ID}/access_as_user`;
