const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('loading', {
  onState: callback => ipcRenderer.on('loading:state', (_event, state) => callback(state))
});
