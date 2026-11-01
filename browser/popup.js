// SPDX-License-Identifier: GPL-3.0-or-later
const api = globalThis.browser || globalThis.chrome;
api.tabs.query({ active: true, currentWindow: true }, tabs => {
  try {
    const source = new URL(tabs[0].url);
    if (!['http:', 'https:'].includes(source.protocol)) throw new Error('This page has no supported media URL.');
    for (const id of ['open', 'enqueue']) {
      document.getElementById(id).href = `velin://open?url=${encodeURIComponent(source.href)}&enqueue=${id === 'enqueue' ? '1' : '0'}`;
    }
  } catch (error) { document.getElementById('status').textContent = error.message; }
});
