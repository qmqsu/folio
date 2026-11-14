#!/usr/bin/env node
// Offline browser tests. No packages are installed and no device APIs are contacted.
// FOLIO_PLAYWRIGHT_MODULE and FOLIO_CHROMIUM_EXECUTABLE can select existing local tools.
import fs from 'node:fs/promises';
import path from 'node:path';
import {homedir} from 'node:os';
import http from 'node:http';
import assert from 'node:assert/strict';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import vm from 'node:vm';
const root=path.resolve(path.dirname(fileURLToPath(import.meta.url)),'../..');
const htmlPath=path.join(root,'main/net/folio_transfer_page.html');
const incPath=path.join(root,'main/net/folio_transfer_page.inc');
const engineName=process.env.FOLIO_WEB_ENGINE==='webkit'?'webkit':'chromium';
const out=path.join(root,'artifacts/firmware-0829/transfer-web',engineName==='webkit'?'webkit':'');
const html=await fs.readFile(htmlPath,'utf8');
const lines=html.match(/[^\n]*\n|[^\n]+$/g)||[];
const generated='/* Generated from folio_transfer_page.html; no external resources. */\nstatic const char transfer_page[] =\n'+lines.map(s=>JSON.stringify(s)).join('\n')+';\n';
new vm.Script(html.match(/<script>([\s\S]*?)<\/script>/)[1]);
if(process.argv.includes('--generate-inc')){await fs.writeFile(incPath,generated);assert.equal(generated.split('\n').slice(2,-1).map(s=>JSON.parse(s.replace(/;$/,''))).join(''),html);console.log('Generated deterministic inc; reverse round-trip matched HTML.');process.exit(0);}
assert.equal(await fs.readFile(incPath,'utf8'),generated,'Generated include must match the HTML');
const require=createRequire(import.meta.url);
let playwright;
for(const candidate of [process.env.FOLIO_PLAYWRIGHT_MODULE,'playwright',path.join(homedir(),'.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules/playwright')].filter(Boolean)){try{playwright=require(candidate);break;}catch{}}
assert.ok(playwright,'Provide an existing Playwright via FOLIO_PLAYWRIGHT_MODULE; this test never installs packages.');
let executablePath=engineName==='webkit'?process.env.FOLIO_WEBKIT_EXECUTABLE:process.env.FOLIO_CHROMIUM_EXECUTABLE;
if(!executablePath){for(const candidate of [playwright[engineName].executablePath(),path.join(homedir(),'Library/Caches/ms-playwright',engineName==='webkit'?'webkit-2359/pw_run.sh':'chromium-1243/chrome-mac-arm64/Google Chrome for Testing.app/Contents/MacOS/Google Chrome for Testing')]){try{await fs.access(candidate);executablePath=candidate;break;}catch{}}}
assert.ok(executablePath,'Provide an existing Chrome executable via FOLIO_CHROMIUM_EXECUTABLE');
await fs.mkdir(out,{recursive:true});
const token='0123456789abcdef0123456789abcdef';
const longName='很长的文件名_阅读与山川_'.repeat(9)+'a'.repeat(100)+'.epub';
const csp="default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src blob:; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
let uploads=[],deletes=[],requests=[],store={},mode='normal',sessionFailures=0,sessionCount=0;
const timers=new Set();
function delay(fn,ms){const t=setTimeout(()=>{timers.delete(t);fn();},ms);timers.add(t);}
function reset(){uploads=[];deletes=[];requests=[];mode='normal';sessionFailures=0;store={books:Array.from({length:20},(_,i)=>({name:i===0?longName:`书籍-${String(i+1).padStart(2,'0')}.txt`,bytes:1024*(i+1)})),picture:[{name:'山间的清晨.jpg',bytes:23456}],fonts:[{name:'我的中文字体.ttf',bytes:3456789}],update:[]};}
reset();
const server=http.createServer(async(req,res)=>{
 const url=new URL(req.url,'http://127.0.0.1');requests.push({method:req.method,path:req.url,token:req.headers['x-folio-token']||null});
 const json=(status,data)=>{if(!res.destroyed){res.writeHead(status,{'Content-Type':'application/json','Cache-Control':'no-store'});res.end(JSON.stringify(data));}};
 if(url.pathname==='/'){res.writeHead(200,{'Content-Type':'text/html; charset=utf-8','Content-Security-Policy':csp});res.end(html);return;}
 if(url.pathname==='/api/session'){sessionCount++;if(sessionFailures-->0){json(503,{error:'设备暂不可用'});return;}json(200,{token});return;}
 if(url.pathname==='/favicon.ico'){res.writeHead(204);res.end();return;}
 if(req.headers['x-folio-token']!==token){json(403,{error:'会话已失效，请重新连接。'});return;}
 if(url.pathname==='/api/files'){const kind=url.searchParams.get('kind'),offset=Number(url.searchParams.get('offset'));if(mode==='list-failure'){json(503,{error:'TF 卡暂不可用'});return;}const files=store[kind]||[];json(200,{files:files.slice(offset,offset+16),more:offset+16<files.length});return;}
 let body=Buffer.alloc(0);try{for await(const chunk of req)body=Buffer.concat([body,chunk]);}catch{return;}
 if(url.pathname==='/api/upload'){
  const name=decodeURIComponent(req.headers['x-folio-name']),folder=req.headers['x-folio-directory'];uploads.push({name,folder,bytes:body.length,token:req.headers['x-folio-token'],type:req.headers['content-type']});
  if(name.includes('损坏')){delay(()=>json(422,{error:'EPUB 校验失败：缺少有效内容。请检查文件。'}),80);return;}
  if(name.includes('忙碌')&&uploads.filter(x=>x.name===name).length===1){json(503,{error:'TF 卡正在使用，请稍后重试。'});return;}
  if(name.includes('过期')){json(403,{error:'会话已失效，请重新连接。'});return;}
  if(name.includes('null响应')){json(200,null);return;}
  if(name.includes('坏响应')){res.writeHead(200,{'Content-Type':'application/json'});res.end('{');return;}
  if((store[folder]||[]).some(f=>f.name===name)){json(409,{error:'同名文件已存在，不会覆盖。'});return;}
  if(!store[folder]){json(400,{error:'目录无效'});return;}
  if(name.includes('停止')){delay(()=>json(200,{ok:true}),2500);return;}
  store[folder].push({name,bytes:body.length});
  if(name.includes('响应丢失')){res.writeHead(200,{'Content-Type':'application/json','Content-Length':'200'});res.write('{');delay(()=>req.socket.destroy(),25);return;}
  delay(()=>json(200,{ok:true,saved_only:/\.(pdf|mobi|azw3|png|jpg|gif|heic)$/i.test(name),message:/\.pdf$/i.test(name)?'文件已原样保存；PDF 暂不能在设备阅读。':'文件已保存。'}),name.includes('慢校验')?900:100);return;
 }
 if(url.pathname==='/api/delete'){const form=new URLSearchParams(body.toString());const name=form.get('name'),kind=form.get('kind');deletes.push({name,kind});store[kind]=store[kind].filter(f=>f.name!==name);json(200,{ok:true});return;}
 json(404,{error:'Not found'});
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const origin=`http://127.0.0.1:${server.address().port}`;
let browser;
try{browser=await playwright[engineName].launch({headless:true,executablePath});}catch(e){server.closeAllConnections();await new Promise(resolve=>server.close(resolve));throw e;}
const results={engine:engineName,executablePath,origin,scope:'Mock API only; no device/network changes',checks:[],layouts:[],pageErrors:[],screenshots:[]};
const check=(name,detail)=>{results.checks.push({name,passed:true,...detail});console.log('PASS',name);};
const payload=(name,content='测试正文'.repeat(100))=>({name,mimeType:'application/octet-stream',buffer:Buffer.from(content)});
let context,page;
async function open(width=390,height=844){if(context)await context.close();context=await browser.newContext({viewport:{width,height},isMobile:width<760,hasTouch:width<760,deviceScaleFactor:1,reducedMotion:'reduce'});page=await context.newPage();page.on('pageerror',e=>results.pageErrors.push(e.message));await page.goto(origin);await page.waitForFunction(()=>document.getElementById('connection').textContent.includes('已连接'));await page.waitForFunction(()=>!document.getElementById('refresh').disabled);return page;}
async function screenshot(name){const file=name+'.png';await page.screenshot({path:path.join(out,file),fullPage:true});results.screenshots.push(file);}
async function queueState(index,text){await page.locator('.queue-item').nth(index).locator('.item-status').filter({hasText:text}).waitFor();}
async function layout(label){const data=await page.evaluate(()=>{
 const w=document.documentElement.clientWidth;
 const overflow=[...document.querySelectorAll('body *')].filter(e=>{const s=getComputedStyle(e),r=e.getBoundingClientRect();return s.display!=='none'&&s.visibility!=='hidden'&&!e.classList.contains('skip')&&!e.classList.contains('sr-only')&&r.width>0&&(r.right>w+1||r.left< -1);}).map(e=>({tag:e.tagName,id:e.id,class:e.className,text:(e.textContent||'').slice(0,70),left:e.getBoundingClientRect().left,right:e.getBoundingClientRect().right}));
 const targets=[...document.querySelectorAll('button,select,input[type=file],summary')].filter(e=>e.getClientRects().length&&!e.closest('[hidden]')).map(e=>({tag:e.tagName,id:e.id,text:e.textContent.slice(0,30),width:e.getBoundingClientRect().width,height:e.getBoundingClientRect().height}));
 return {width:w,scrollWidth:document.documentElement.scrollWidth,overflow,smallTargets:targets.filter(t=>t.width<44||t.height<44),bodyFont:getComputedStyle(document.body).fontSize,logWidth:document.getElementById('queue-scroll').clientWidth,logScrollWidth:document.getElementById('queue-scroll').scrollWidth,logHeight:document.getElementById('queue-scroll').clientHeight,logScrollHeight:document.getElementById('queue-scroll').scrollHeight};
 });results.layouts.push({label,...data});assert.equal(data.scrollWidth,data.width,label+' document overflow');assert.deepEqual(data.overflow,[],label+' element overflow');assert.deepEqual(data.smallTargets,[],label+' touch targets');assert.equal(data.bodyFont,'16px');assert.ok(data.logHeight>=440);assert.equal(data.logWidth,data.logScrollWidth,label+' internal log horizontal overflow');}
try{
 for(const width of [320,390,768,1440]){
  reset();await open(width,width===1440?1000:844);assert.equal(await page.locator('#login, #login-form, #code').count(),0);assert.equal(await page.getByText('访问码').count(),0);
  await page.locator('#pick').setInputFiles([payload(longName),payload('损坏的书.epub'),payload('慢校验-旅行札记.txt'),payload('存档.pdf'),payload('动画.gif')]);
  await layout('queued-'+width);await screenshot('queued-'+width);
  await page.locator('#upload').click();await queueState(2,'服务端校验中');assert.equal(await page.locator('.queue-item').nth(2).locator('.item-status').innerText(),'服务端校验中');
  await page.locator('.queue-item').nth(2).scrollIntoViewIfNeeded();await screenshot('validating-'+width);
  await queueState(4,'上传成功');await page.waitForFunction(()=>!document.getElementById('refresh').disabled);
  assert.equal(uploads.length,5);assert.equal(uploads[4].folder,'picture');assert.equal(uploads[3].folder,'books');assert.equal(uploads.every(x=>x.token===token&&x.type==='application/octet-stream'),true);
  assert.match(await page.locator('.queue-item').nth(0).innerText(),/上传失败|不会覆盖/); // longName already exists
  assert.match(await page.locator('.queue-item').nth(1).innerText(),/EPUB 校验失败/);
  assert.match(await page.locator('.queue-item').nth(3).innerText(),/仅保存|暂不能/);
  await page.locator('#queue-scroll').evaluate(e=>e.scrollTop=0);await layout('results-'+width);await screenshot('results-'+width);
  check('responsive / long names / multi-file failure continuation / validating '+width);
 }
 reset();await open();const initialSessions=sessionCount;
 assert.equal(requests.some(r=>r.path.includes('/api/login')),false);assert.equal(await page.evaluate(()=>location.hash),'');assert.equal(await page.evaluate(()=>localStorage.length),0);assert.ok(initialSessions>0);check('automatic session and token in request headers only');
 await page.keyboard.press(engineName==='webkit'?'Alt+Tab':'Tab');assert.match(await page.locator(':focus').innerText(),/跳到上传/);await page.keyboard.press('Enter');assert.equal(await page.evaluate(()=>document.activeElement.id),'main');await page.keyboard.press('Tab');assert.equal(await page.evaluate(()=>document.activeElement.id),'destination');assert.equal(await page.locator('#destination').evaluate(e=>getComputedStyle(e).outlineStyle),'solid');check('keyboard skip link, natural tab order, visible focus');
 await page.locator('#more').click();await page.waitForFunction(()=>document.getElementById('page-number').textContent==='第 2 页');assert.equal(await page.locator('.file-row').count(),4);await page.locator('#prev').click();await page.waitForFunction(()=>document.getElementById('page-number').textContent==='第 1 页');
 page.once('dialog',d=>d.dismiss());await page.locator('[data-delete]').nth(1).click();assert.equal(deletes.length,0);page.once('dialog',d=>d.accept());await page.locator('[data-delete]').nth(1).click();await page.waitForFunction(()=>document.getElementById('message').textContent.includes('已删除'));assert.equal(deletes.length,1);
 await page.locator('#kind').selectOption('fonts');await page.waitForFunction(()=>document.getElementById('files-status').textContent.includes('fonts ·'));assert.equal(await page.locator('[data-delete]').count(),0);assert.match(await page.locator('#files').innerText(),/设备上删除/);check('mobile paging / delete confirmation / font deletion remains on device');
 mode='list-failure';await page.locator('#refresh').click();await page.waitForFunction(()=>document.getElementById('files-status').textContent.includes('读取失败'));assert.equal(await page.locator('#more').isEnabled(),false);mode='normal';await page.locator('#refresh').click();await page.waitForFunction(()=>document.getElementById('files-status').textContent.includes('fonts ·'));check('list failure feedback and recovery');
 reset();await open();await page.locator('#pick').setInputFiles([payload('忙碌稍后.txt'),payload('下一本.txt')]);await page.locator('#upload').click();await queueState(1,'上传成功');await page.locator('[data-action=retry]').first().waitFor();await page.waitForFunction(()=>!document.querySelector('[data-action=retry]').disabled);await page.locator('[data-action=retry]').first().click();assert.equal(uploads.length,2);await page.locator('#upload').click();await queueState(0,'上传成功');assert.equal(uploads.length,3);check('explicit retry requires user start and retains individual attempt count');
 reset();await open();await page.locator('#pick').setInputFiles([payload('响应丢失已提交.txt'),payload('断线后的下一本.txt')]);await page.locator('#upload').click();await queueState(0,'结果待核对');assert.equal(uploads.length,1);assert.equal(store.books.some(f=>f.name==='响应丢失已提交.txt'),true);
 assert.equal(await page.locator('[data-action=unknownRetry]').first().isDisabled(),true);await page.locator('#reconnect').click();await page.waitForFunction(()=>document.getElementById('connection').textContent.includes('已连接'));assert.equal(uploads.length,1);
 await page.locator('[data-action=check]').first().click();await page.waitForFunction(()=>!document.querySelector('[data-action=unknownRetry]').disabled);assert.equal(uploads.length,1);page.once('dialog',d=>d.dismiss());await page.locator('[data-action=unknownRetry]').first().click();assert.equal(uploads.length,1);assert.match(await page.locator('.queue-item').first().innerText(),/结果待核对/);
 await screenshot('unknown-response-390');await page.locator('#upload').click();await queueState(1,'上传成功');assert.equal(uploads.length,2);assert.equal(uploads.filter(u=>u.name==='响应丢失已提交.txt').length,1);check('committed response loss is quarantined; no replay after reconnect or resume');
 reset();await open();await page.locator('#pick').setInputFiles([payload('待取消.txt'),payload('停止传输.txt'),payload('取消后的文件.txt')]);await page.locator('[data-action=cancel]').first().click();await queueState(0,'已取消');await page.locator('#upload').click();await queueState(1,'服务端校验中');page.once('dialog',d=>d.accept());await page.locator('.queue-item').nth(1).locator('[data-action=cancel]').click();await queueState(1,'结果待核对');assert.equal(uploads.length,1);assert.equal(await page.locator('.queue-item').nth(2).locator('.item-status').innerText(),'等待上传');check('queued cancel sends nothing; active cancel remains unknown and pauses later files');
 reset();await open();await page.locator('#pick').setInputFiles([payload('慢校验-暂停.txt'),payload('暂停之后.txt')]);await page.locator('#upload').click();await queueState(0,'服务端校验中');await page.locator('#pause').click();await queueState(0,'上传成功');assert.equal(uploads.length,1);await page.waitForFunction(()=>!document.getElementById('upload').disabled);await page.locator('#upload').click();await queueState(1,'上传成功');check('pause completes in-flight file and explicitly resumes remaining files');
 reset();await open();await page.locator('#pick').setInputFiles(payload('慢校验-固定批次.txt'));await page.locator('#upload').click();await queueState(0,'服务端校验中');await page.locator('#pick').setInputFiles(payload('运行中新添加.txt'));await queueState(0,'上传成功');await page.waitForFunction(()=>!document.getElementById('upload').disabled);assert.equal(uploads.length,1);assert.equal(await page.locator('.queue-item').nth(1).locator('.item-status').innerText(),'等待上传');await page.locator('#upload').click();await queueState(1,'上传成功');assert.equal(uploads.length,2);check('new files added during a run wait for separate reviewed start');
 reset();await open();await page.locator('#pick').setInputFiles([payload('null响应.txt'),payload('坏响应.txt'),payload('依然继续.txt')]);await page.locator('#upload').click();await queueState(2,'上传成功');await queueState(0,'结果待核对');await queueState(1,'结果待核对');assert.equal(uploads.length,3);check('malformed or null 2xx is not reported as success');
 reset();await open();await page.locator('#pick').setInputFiles([payload('过期.txt'),payload('过期之后.txt')]);await page.locator('#upload').click();await queueState(0,'上传失败');assert.equal(uploads.length,1);await page.locator('#reconnect').click();await page.waitForFunction(()=>document.getElementById('connection').textContent.includes('已连接'));assert.equal(uploads.length,1);check('expired session reconnect never repeats write automatically');
 reset();await open();await page.locator('#pick').setInputFiles(payload('离线待传.txt'));await context.setOffline(true);await page.waitForFunction(()=>document.getElementById('connection').textContent.includes('待恢复'));assert.equal(await page.locator('#upload').isDisabled(),true);await context.setOffline(false);assert.equal(uploads.length,0);await page.locator('#reconnect').click();await page.waitForFunction(()=>document.getElementById('connection').textContent.includes('已连接'));assert.equal(uploads.length,0);check('offline/online does not silently start queued writes');
 // Create an actual browser-encoded PNG fixture; upload conversion decodes and re-encodes it.
 reset();await open();const pngBase64=await page.evaluate(()=>{const c=document.createElement('canvas');c.width=2000;c.height=3000;const x=c.getContext('2d');x.fillStyle='#ddd';x.fillRect(0,0,2000,3000);x.fillStyle='#222';x.font='180px serif';x.fillText('Folio',200,500);x.fillRect(150,800,1700,3);return c.toDataURL('image/png').split(',')[1];});
 const png=Buffer.from(pngBase64,'base64');await fs.writeFile(path.join(out,'fixture-large.png'),png);
 await page.locator('#image-options summary').click();await page.locator('#adapt').check();await page.locator('#pick').setInputFiles([{name:'本地大图.png',mimeType:'image/png',buffer:png},payload('动画.gif'),payload('手机.heic')]);await page.waitForFunction(()=>document.querySelector('.preview img')?.complete&&document.querySelectorAll('[data-action=original]:not([hidden])').length===2);
 assert.match(await page.locator('.queue-item').first().innerText(),/684 × 1026/);assert.match(await page.locator('.queue-item').first().innerText(),/本地大图-folio.jpg/);assert.equal(uploads.length,0);await screenshot('adapt-preview-390');
 await page.locator('.queue-item').nth(1).locator('[data-action=original]').click();await page.locator('.queue-item').nth(2).locator('[data-action=original]').click();await page.locator('#upload').click();await queueState(2,'上传成功');assert.equal(uploads[0].name,'本地大图-folio.jpg');assert.equal(uploads[0].folder,'picture');assert.ok(uploads[0].bytes<2*1024*1024);assert.equal(uploads[1].bytes,Buffer.from('测试正文'.repeat(100)).length);check('real image decode / resized copy / preview / unsupported GIF and HEIC explicit original fallback');
 // APNG acTL is parsed before browser decode; fake CRC does not bypass static-image detection.
 const actl=Buffer.alloc(20);actl.writeUInt32BE(8,0);actl.write('acTL',4);actl.writeUInt32BE(2,8);const apng=Buffer.concat([png.subarray(0,33),actl,png.subarray(33)]);
 await page.locator('#pick').setInputFiles({name:'动画图.png',mimeType:'image/png',buffer:apng});await queueState(3,'适配未完成');assert.match(await page.locator('.queue-item').nth(3).innerText(),/动画 PNG/);check('animated PNG never silently flattened');
 // Explicit books cover destination and all upload-directory labels use real directory names.
 reset();await open(320);await page.locator('#destination').selectOption('books');await page.locator('#pick').setInputFiles({name:'封面.png',mimeType:'image/png',buffer:png});await page.locator('#upload').click();await queueState(0,'上传成功');assert.equal(uploads[0].folder,'books');await page.locator('#image-options summary').click();await layout('expanded-320');await screenshot('expanded-320');check('explicit books image target and expanded narrow-screen layout');
 // 200% text with tablet-wide CSS viewport, landscape, and reduced motion.
 await page.setViewportSize({width:768,height:390});await page.addStyleTag({content:'html{font-size:32px}'});const enlarged=await page.evaluate(()=>({width:document.documentElement.clientWidth,scroll:document.documentElement.scrollWidth}));assert.equal(enlarged.width,enlarged.scroll);await screenshot('large-text-landscape');check('200-percent text reflow and short landscape');
 reset();store.picture=[{name:'<img src=x onerror=window.pwned=1>.png',bytes:1234}];await open();await page.locator('#kind').selectOption('picture');await page.waitForFunction(()=>document.getElementById('files-status').textContent.includes('picture ·'));assert.equal(await page.locator('#files img').count(),0);assert.equal(await page.evaluate(()=>window.pwned),undefined);assert.match(await page.locator('#files').innerText(),/<img src=x/);check('hostile filenames render as text, not HTML');
 for(const width of [390,1440]){reset();store.books=[{name:'山间日记.epub',bytes:2400123},{name:'沿着河流走.txt',bytes:168290},{name:'植物观察手记.pdf',bytes:6812345}];await open(width,width===1440?1000:844);await page.locator('#pick').setInputFiles([payload('慢校验-新的旅程.epub'),payload('损坏-待修复.epub'),payload('旧杂志存档.pdf'),payload('晴日照片.jpg')]);await page.locator('#upload').click();await queueState(3,'上传成功');await page.waitForFunction(()=>!document.getElementById('refresh').disabled);await page.locator('#queue-scroll').evaluate(e=>e.scrollTop=0);await layout('overview-'+width);await screenshot('overview-'+width);}
 check('representative desktop and mobile overview screenshots');
 assert.deepEqual(results.pageErrors,[]);check('no uncaught browser JavaScript errors');
 results.passed=true;
}catch(e){results.passed=false;results.failure=e.stack;try{await screenshot('failure');}catch{};throw e;}
finally{
 await fs.writeFile(path.join(out,'results.json'),JSON.stringify(results,null,2)+'\n');
 if(context)await context.close();await browser.close();for(const t of timers)clearTimeout(t);server.closeAllConnections();await new Promise(resolve=>server.close(resolve));
 console.log('Mock server and browser closed. Evidence:',out);
}
