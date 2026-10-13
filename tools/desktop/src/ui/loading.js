window.loading.onState(state => {
  document.body.classList.toggle('active', state.animate);
  document.body.classList.toggle('warning', state.warning);
  document.getElementById('title').textContent = state.title;
  document.getElementById('detail').textContent = state.detail;
  document.getElementById('completed').textContent = state.completed.toLocaleString();
  document.getElementById('elapsed').textContent = state.elapsed >= 60 ? `${Math.floor(state.elapsed / 60)}m ${state.elapsed % 60}s` : `${state.elapsed}s`;
  document.getElementById('activity').textContent = state.warning ? 'WAITING FOR PROGRESS' : 'RENDERER ACTIVITY';
  document.getElementById('note').textContent = state.failed ? `${state.failed} failed` : 'Total discovered during play';
});
