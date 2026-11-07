const originals = new WeakMap();
let preference = 'auto';
try { preference = localStorage.getItem('qo100.language') || 'auto'; } catch {}
export function resolveLanguage(value, browser = navigator.languages?.[0] || navigator.language || 'en') {
  return value === 'de' || value === 'en' ? value : /^de(?:-|$)/i.test(browser) ? 'de' : 'en';
}
export let language = resolveLanguage(preference);
export const text = (de, en) => language === 'en' ? en : de;
export const languagePreference = () => ['de', 'en'].includes(preference) ? preference : 'auto';

export function applyLanguage() {
  document.documentElement.lang = language;
  for (const element of document.querySelectorAll('[data-en], [data-en-title], [data-en-aria-label]')) {
    if (!originals.has(element)) originals.set(element, {
      nodes: [...element.childNodes].filter(node => node.nodeType === 3 && node.textContent.trim()).map(node => [node, node.textContent]),
      title: element.getAttribute('title'), aria: element.getAttribute('aria-label'),
    });
    const source = originals.get(element);
    if (element.hasAttribute('data-en')) for (const [node, de] of source.nodes) {
      node.textContent = language === 'en' ? de.replace(de.trim(), element.dataset.en) : de;
    }
    for (const [attribute, key, de] of [['title', 'enTitle', source.title], ['aria-label', 'enAriaLabel', source.aria]]) {
      if (element.dataset[key] !== undefined) element.setAttribute(attribute, language === 'en' ? element.dataset[key] : de);
    }
  }
}

export function setLanguagePreference(value) {
  preference = ['de', 'en'].includes(value) ? value : 'auto';
  try { localStorage.setItem('qo100.language', preference); } catch {}
  language = resolveLanguage(preference);
  applyLanguage();
}
