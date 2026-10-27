// Renders mockup pages in headless Chromium (WebGL via SwiftShader) and saves screenshots.
// usage: node render.js                       -> out/TCOLD_mockup.png (3840x2160)
//        node render.js '[["hero.html?pr=1.2","x.png",1920,1080,1]]'
// a job is [page, out, width, height, deviceScaleFactor, transparent]
const {chromium}=require('playwright'); const http=require('http'),fs=require('fs'),path=require('path');
const root=path.join(__dirname,'..'); const types={'.html':'text/html','.js':'text/javascript','.png':'image/png','.jpg':'image/jpeg','.stl':'application/octet-stream','.svg':'image/svg+xml','.ttf':'font/ttf','.otf':'font/otf','.woff2':'font/woff2'};
const srv=http.createServer((q,res)=>{const f=path.join(root,decodeURIComponent(q.url.split('?')[0]));fs.readFile(f,(e,d)=>{if(e){res.writeHead(404);res.end();return}res.writeHead(200,{'Content-Type':types[path.extname(f)]||'application/octet-stream'});res.end(d)})}).listen(8765);
(async()=>{const b=await chromium.launch({args:['--use-gl=angle','--use-angle=swiftshader','--enable-unsafe-swiftshader']});
const jobs=JSON.parse(process.argv[2]||'[["hero.html","'+path.join(__dirname,'out/TCOLD_mockup.png').replace(/\\/g,'/')+'",1920,1080,2]]');
for(const [url,out,w,h,dpr,clear] of jobs){const pg=await b.newPage({viewport:{width:w||800,height:h||800},deviceScaleFactor:dpr||1});pg.on('console',m=>console.log('console:',m.text()));pg.on('pageerror',e=>console.log('err:',e.message));
await pg.goto('http://localhost:8765/mockup/'+url);await pg.waitForFunction('window.done',null,{timeout:240000});await pg.screenshot({path:out,omitBackground:!!clear});await pg.close();console.log('ok',out)}
await b.close();srv.close()})();
