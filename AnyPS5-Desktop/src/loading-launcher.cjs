const { app } = require('electron');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { createLoadingOverlay } = require('./loading-overlay.cjs');

async function launchWithLoadingOverlay(executable, profile) {
  const loading = await createLoadingOverlay(profile, { title: path.basename(path.dirname(executable)), executable });
  try {
    await new Promise((resolve, reject) => {
      const child = spawn(executable, [], { cwd: path.dirname(executable), env: { ...process.env, ANYPS5_LOADING_STATUS: loading.statusPath }, stdio: 'inherit', shell: false });
      child.once('error', reject);
      child.once('spawn', resolve);
      child.once('exit', (code, signal) => {
        loading.stop(true, code === 0 && !signal ? 'exited' : 'interrupted').finally(() => app.quit()).catch(() => {});
      });
    });
  } catch (error) {
    await loading.stop(true, 'launch-failed');
    throw error;
  }
}
module.exports = { launchWithLoadingOverlay };
