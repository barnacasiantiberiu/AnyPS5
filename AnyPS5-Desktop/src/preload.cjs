const { contextBridge, ipcRenderer } = require('electron');
const invoke = async (name, ...args) => {
  const response = await ipcRenderer.invoke(name, ...args);
  if (!response.ok) { const error = new Error(response.error); error.log = response.log; throw error; }
  return response.value;
};
contextBridge.exposeInMainWorld('desktop', {
  state: () => invoke('state:get'),
  windowVisibility: () => invoke('window:visibility'),
  onWindowVisibility: callback => { const listener = (_event, value) => callback(value); ipcRenderer.on('window:visibility-changed', listener); return () => ipcRenderer.removeListener('window:visibility-changed', listener); },
  fullscreen: () => invoke('window:fullscreen'),
  close: () => invoke('window:close'),
  onFullscreen: callback => { const listener = (_event, value) => callback(value); ipcRenderer.on('window:fullscreen-changed', listener); return () => ipcRenderer.removeListener('window:fullscreen-changed', listener); },
  pick: type => invoke('dialog:pick', type),
  inspect: folder => invoke('source:inspect', folder),
  inspectIso: image => invoke('source:inspect-iso', image),
  checkEntry: request => invoke('source:check-entry', request),
  saveSettings: settings => invoke('settings:save', settings),
  preflight: request => invoke('installation:preflight', request),
  install: request => invoke('installation:start', request),
  cancel: () => invoke('installation:cancel'),
  artwork: id => invoke('game:artwork', id),
  launch: id => invoke('game:launch', id),
  folder: id => invoke('game:folder', id),
  shortcut: id => invoke('game:shortcut', id),
  remove: id => invoke('game:remove', id),
  importGame: directory => invoke('game:import', directory),
  onProgress: callback => { const listener = (_event, value) => callback(value); ipcRenderer.on('installation:event', listener); return () => ipcRenderer.removeListener('installation:event', listener); }
});
