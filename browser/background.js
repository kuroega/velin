// SPDX-License-Identifier: GPL-3.0-or-later
const api = globalThis.browser || globalThis.chrome;
function playerUrl(source, enqueue = false) {
  const url = new URL(source);
  if (!['http:', 'https:'].includes(url.protocol)) throw new Error('Choose an HTTP or HTTPS media link.');
  return `velin://open?url=${encodeURIComponent(url.href)}&enqueue=${enqueue ? '1' : '0'}`;
}
api.runtime.onInstalled.addListener(() => {
  api.contextMenus.removeAll(() => {
    for (const [id, title] of [['open', 'Open in Velin'], ['enqueue', 'Add to Velin playlist']]) {
      api.contextMenus.create({ id, title, contexts: ['page', 'link', 'video', 'audio'] });
    }
  });
});
api.contextMenus.onClicked.addListener((info, tab) => {
  try {
    const url = playerUrl(info.srcUrl || info.linkUrl || info.pageUrl || tab.url, info.menuItemId === 'enqueue');
    api.tabs.create({ url });
  } catch (error) { console.error(error.message); }
});
