const { test } = require('node:test');
const assert = require('node:assert/strict');
const { createSampler } = require('../src/ui/controller-navigation.js');
function pad(index = 0) { return { index, id: 'Xbox 360 Controller', connected: true, mapping: 'standard', axes: [0,0,0,0], buttons: Array.from({length:17},()=>({pressed:false,value:0})) }; }
function button(pad,index,on=true) { pad.buttons[index]={pressed:on,value:on?1:0}; }
test('D-pad/left stick navigate; A confirms and B backs out on edges only', () => {
  const input=createSampler(), p=pad(); assert.equal(input.sample([p],0),null);
  for(const [index,key] of [[15,'ArrowRight'],[14,'ArrowLeft'],[12,'ArrowUp'],[13,'ArrowDown'],[0,'Enter'],[1,'Escape']]) {
    button(p,index);assert.equal(input.sample([p],100).key,key);button(p,index,false);assert.equal(input.sample([p],110),null);
  }
  button(p,0); assert.equal(input.sample([p],200).key,'Enter'); assert.equal(input.sample([p],2000),null);
  button(p,0,false);input.sample([p],2010);p.axes=[-.8,0,0,0];assert.equal(input.sample([p],2020).key,'ArrowLeft');
});
test('drift is ignored, hysteresis prevents jitter, and repeat is bounded', () => {
  const input=createSampler(),p=pad();input.sample([p],0);p.axes=[.3,-.2];assert.equal(input.sample([p],10),null);
  p.axes=[.8,0];assert.equal(input.sample([p],20).key,'ArrowRight');p.axes=[.5,0];assert.equal(input.sample([p],399),null);
  assert.equal(input.sample([p],400).key,'ArrowRight');assert.equal(input.sample([p],514),null);assert.equal(input.sample([p],515).key,'ArrowRight');
  p.axes=[.2,0];assert.equal(input.sample([p],520),null);p.axes=[.5,0];assert.equal(input.sample([p],600),null);
  p.axes=[0,-.9];assert.equal(input.sample([p],610).key,'ArrowUp');
});
test('connect, reconnect, and focus restore require a neutral frame before activation', () => {
  const input=createSampler(),p=pad();button(p,0);assert.equal(input.sample([p],0),null);assert.equal(input.sample([p],500),null);
  button(p,0,false);input.sample([p],510);button(p,0);assert.equal(input.sample([p],520).key,'Enter');
  assert.equal(input.sample([p],530,false),null);assert.equal(input.sample([p],540),null);button(p,0,false);input.sample([p],550);button(p,0);assert.equal(input.sample([p],560).key,'Enter');
  input.sample([],570);assert.equal(input.sample([p],580),null);
});
test('unknown mappings never guess activation, any standard pad can take control, and B wins simultaneous A', () => {
  const input=createSampler(),p=pad(0),q=pad(2);p.mapping='';input.sample([p,q],0);button(p,0);assert.equal(input.sample([p,q],20),null);
  button(q,0);button(q,1);assert.equal(input.sample([p,q],30).key,'Escape');assert.equal(input.sample([p,q],500),null);
  button(q,0,false);button(q,1,false);input.sample([p,q],510);button(q,15);assert.equal(input.sample([p,q],520).index,2);
});
test('two pads cannot emit duplicate confirmation or queue the second held press', () => {
  const input=createSampler(),p=pad(0),q=pad(1);input.sample([p,q],0);button(p,0);button(q,0);
  assert.equal(input.sample([p,q],20).key,'Enter');assert.equal(input.sample([p,q],30),null);
});
