const duration = seconds => seconds >= 60 ? `${Math.floor(seconds / 60)}m ${seconds % 60}s` : `${seconds}s`;
window.loading.onState(state => {
  document.body.classList.toggle('active', state.animate);
  document.body.classList.toggle('warning', state.warning);
  document.getElementById('title').textContent = state.title;
  document.getElementById('detail').textContent = state.detail;
  document.getElementById('completed').textContent = state.completed.toLocaleString();
  document.getElementById('elapsed').textContent = duration(state.elapsed);
  document.getElementById('activity').textContent = state.warning ? 'WAITING FOR PROGRESS' : state.active ? 'RENDERER ACTIVITY' : 'RENDERING';
  document.getElementById('note').textContent = state.failed ? `${state.failed} failed` : 'Total discovered during play';
  document.getElementById('last-progress').textContent = `${state.active} active · ${state.completed + state.failed ? 'Last completion' : 'No completion for'} ${duration(state.quietSeconds)}${state.completed + state.failed ? ' ago' : ''}`;
  const previous = state.previous;
  document.getElementById('previous').textContent = state.historyError ? 'Timing history could not be saved' : previous ? `Previous run: last completion at ${duration(Math.floor(previous.lastCompletedMs / 1000))} · ${previous.completed} steps${previous.status === 'exited' ? '' : ' · partial'}` : 'Timing history starts with this run';
});
