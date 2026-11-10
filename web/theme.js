// Apply before the stylesheet loads to avoid flashing the wrong page theme.
(() => {
  const key = 'qo100.theme';
  const normalize = value => value === 'light' ? 'light' : 'dark';
  function apply(value) {
    const theme = normalize(value);
    document.documentElement.dataset.theme = theme;
    const meta = document.querySelector('meta[name="color-scheme"]');
    if (meta) meta.content = theme;
    const control = document.getElementById('theme');
    if (control) control.checked = theme === 'light';
    return theme;
  }
  let stored;
  try { stored = localStorage.getItem(key); } catch {}
  apply(stored);
  window.QO100Theme = Object.freeze({
    get value() { return document.documentElement.dataset.theme; },
    set(value) {
      const theme = apply(value);
      try { localStorage.setItem(key, theme); } catch {}
    },
  });
  window.addEventListener('storage', event => {
    if (event.key === key || event.key === null) apply(event.newValue);
  });
})();
