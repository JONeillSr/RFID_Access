import { render } from 'preact';
import { useEffect, useState } from 'preact/hooks';
import { Router, Route, Link, useLocation, Switch } from 'wouter-preact';
import { initAuth, signIn, signOut, currentAccount, effectiveRole, atLeast } from './auth';
import { BRAND } from './branding';
import { Dashboard } from './pages/Dashboard';
import { Doors } from './pages/Doors';
import { People } from './pages/People';
import { Groups } from './pages/Groups';
import { Reports } from './pages/Reports';
import './styles.css';

function Logo({ cls }) {
  return (
    <img
      class={cls}
      src={BRAND.logo}
      alt={`${BRAND.company} logo`}
      onError={(e) => { e.currentTarget.style.display = 'none'; }}
    />
  );
}

function Nav() {
  const [loc] = useLocation();
  // Groups is Admin-only and rarely visited: changing a group changes who can
  // open what, so it stays out of the way rather than sitting beside the daily
  // tasks. The route is still enforced server-side regardless of this list.
  const items = [
    ['/', 'Dashboard'],
    ['/doors', 'Doors'],
    ['/people', 'People'],
    ...(atLeast('Admin') ? [['/groups', 'Groups']] : []),
    ['/reports', 'Reports'],
  ];
  return (
    <nav>
      {items.map(([href, label]) => (
        <Link key={href} href={href}
              class={loc === href || (href !== '/' && loc.startsWith(href)) ? 'active' : ''}>
          {label}
        </Link>
      ))}
    </nav>
  );
}

function App() {
  const [ready, setReady] = useState(false);
  const [account, setAccount] = useState(null);
  const [msg, setMsg] = useState(null);

  useEffect(() => {
    initAuth().then((a) => { setAccount(a); setReady(true); });
  }, []);

  /** Turn an API error into something the user can act on. */
  const notify = (e) => {
    if (e?.status === 403) setMsg({ kind: 'warn', text: `Not permitted: ${e.message}` });
    else if (e?.status === 401) setMsg({ kind: 'warn', text: 'Session expired — sign in again.' });
    else setMsg({ kind: 'error', text: e?.message ?? 'Something went wrong' });
  };

  /**
   * Confirm a write succeeded.
   *
   * Auto-dismissed, unlike errors: a success message that lingers gets clicked
   * away reflexively, which trains people to dismiss the ones that matter.
   */
  const flash = (text) => {
    setMsg({ kind: 'ok', text });
    setTimeout(() => setMsg((m) => (m?.text === text ? null : m)), 6000);
  };

  if (!ready) return null;

  if (!account) {
    return (
      <div class="centered">
        <Logo cls="logo" />
        <h1>{BRAND.company}</h1>
        <p class="muted">
          {BRAND.product} — sign in with your work account to manage doors, fobs
          and reports.
        </p>
        <button class="primary" onClick={signIn}>Sign in</button>
      </div>
    );
  }

  const role = effectiveRole();
  if (!role) {
    // Authenticated but in none of the groups. Say exactly what to do, rather
    // than showing an empty app or bouncing back to a login that cannot help.
    return (
      <div class="centered">
        <h1>No access assigned</h1>
        <p class="muted">
          Signed in as <strong>{account.username}</strong>, but your account is not in
          any access-control group, so there is nothing to show you.
        </p>
        <p class="muted">
          Ask an administrator to add you to <code>JTC Access Control</code> for
          read-only access, or one of the Operators / Admins groups.
        </p>
        <p class="muted">
          Group membership is read at sign-in — if you were just added, sign out and
          back in.
        </p>
        <button class="primary" onClick={signOut}>Sign out</button>
      </div>
    );
  }

  return (
    <Router>
      <header>
        <div class="brand">
          <Logo cls="logo" />
          <div class="names">
            <div class="co">{BRAND.company}</div>
            <div class="pd">{BRAND.product}</div>
          </div>
        </div>
        <div class="spacer" />
        <span class="who">{account.username} · {role}</span>
        <button onClick={signOut}>Sign out</button>
      </header>
      <Nav />
      {msg && <div class={`status ${msg.kind}`} onClick={() => setMsg(null)}>{msg.text}</div>}
      <main>
        <Switch>
          <Route path="/"><Dashboard notify={notify} flash={flash} /></Route>
          <Route path="/doors"><Doors notify={notify} flash={flash} /></Route>
          <Route path="/people"><People notify={notify} flash={flash} /></Route>
          <Route path="/groups"><Groups notify={notify} flash={flash} /></Route>
          <Route path="/reports"><Reports notify={notify} /></Route>
          <Route><p class="muted">Page not found.</p></Route>
        </Switch>
      </main>
      <footer>
        <span>{BRAND.company}</span>
        <span class="sep">|</span>
        <span>{BRAND.address}</span>
        <span class="sep">|</span>
        <a href={`tel:${BRAND.phone.replace(/[^0-9+]/g, '')}`}>{BRAND.phone}</a>
      </footer>
    </Router>
  );
}

render(<App />, document.getElementById('app'));
