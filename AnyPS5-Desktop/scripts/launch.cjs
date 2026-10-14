const path = require('node:path');
const { spawn } = require('node:child_process');

function desktopLaunchOptions(args = [], environment = process.env) {
  const env = { ...environment };
  for (const key of Object.keys(env)) {
    if (key.toUpperCase() === 'ELECTRON_RUN_AS_NODE') delete env[key];
  }
  const executable = env.ANYPS5_DESKTOP_EXE;
  if (executable && !path.isAbsolute(executable)) throw new Error('ANYPS5_DESKTOP_EXE requires an absolute executable path.');
  return {
    executablePath: executable || require('electron'),
    args: executable ? [...args] : [path.resolve(__dirname, '..'), ...args],
    env
  };
}

if (require.main === module) {
  try {
    const options = desktopLaunchOptions(process.argv.slice(2));
    const child = spawn(options.executablePath, options.args, { env: options.env, stdio: 'inherit', shell: false });
    child.once('error', error => { console.error(error.message); process.exitCode = 1; });
    child.once('exit', (code, signal) => { process.exitCode = signal ? 1 : code ?? 1; });
  } catch (error) {
    console.error(error.message);
    process.exitCode = 1;
  }
}

module.exports = { desktopLaunchOptions };
