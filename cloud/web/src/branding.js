/**
 * Brand and company details.
 *
 * ONE file, deliberately. Everything identifying the customer lives here and in
 * the :root block of styles.css -- so rebranding is editing two known places,
 * not hunting through markup.
 *
 * Values are baked in at build time. See the note in cloud/web/README.md about
 * when it is worth promoting this to a runtime-configurable admin page: the
 * short version is that it is worth it if this app will ever serve more than one
 * company, and over-engineering if it will not.
 */
export const BRAND = {
  company: 'JT Custom Trailers',
  product: 'Access Control',
  address: '1085 Jefferson-Eagleville Rd., Jefferson OH 44047',
  phone: '(440) 209-2866',
  // Drop the file at cloud/web/public/logo.svg. The header degrades to a text
  // wordmark if it is missing, so a missing asset never breaks the page.
  logo: '/logo.svg',
};
