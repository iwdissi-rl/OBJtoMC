/* VOXFORGE showcase — cinematic, data-driven visualisation of a conversion.
   Every number and voxel on screen is real: mesh points from /api/mesh,
   palette from /api/palette, voxels/chunks from /api/voxels, stats parsed
   from the live objtomc log. The choreography stretches a fast conversion
   into a watchable ~3-minute sequence (duration configurable); phases wait
   on real data when the pipeline is slower than the show. */
"use strict";

const SC = (() => {

const PH = [                       // [key, label, weight]
  ["init",  "INIT",          0.06],
  ["mesh",  "MESH INGEST",   0.15],
  ["calib", "CALIBRATION",   0.10],
  ["pal",   "PALETTE SYNC",  0.12],
  ["voxel", "VOXELIZATION",  0.27],
  ["clean", "REFINE",        0.08],
  ["asm",   "REGION ASSEMBLY",0.14],
  ["done",  "WORLD FORGED",  0.08],
];

// ---- real Oklab (Björn Ottosson) — same math as the matcher ------------
function srgb2lin(c){return c<=0.04045? c/12.92 : Math.pow((c+0.055)/1.055,2.4);}
function oklab(r,g,b){
  const lr=srgb2lin(r),lg=srgb2lin(g),lb=srgb2lin(b);
  const l=Math.cbrt(0.4122214708*lr+0.5363325363*lg+0.0514459929*lb);
  const m=Math.cbrt(0.2119034982*lr+0.6806995451*lg+0.1073969566*lb);
  const s=Math.cbrt(0.0883024619*lr+0.2817188376*lg+0.6299787005*lb);
  return [0.2104542553*l+0.7936177850*m-0.0040720468*s,
          1.9779984951*l-2.4285922050*m+0.4505937099*s,
          0.0259040371*l+0.7827717662*m-0.8086757660*s];
}

const $=s=>document.querySelector(s);
let three={}, state=null;

function el(tag, cls, parent, html){
  const e=document.createElement(tag); if(cls)e.className=cls;
  if(html!==undefined)e.innerHTML=html; (parent||document.body).appendChild(e); return e;
}

// ---------------- log parsing → real events ------------------------------
function parseLine(state,l){
  let m;
  if((m=l.match(/loaded (\d+) positions, (\d+) faces, (\d+) materials/)))
    state.mesh={pos:+m[1],faces:+m[2],mats:+m[3]};
  if((m=l.match(/target_size=(\d+) blocks -> voxel_size=([\d.]+)/)))
    state.calib={target:+m[1],vs:+m[2]};
  if((m=l.match(/grid dims = (\d+) x (\d+) x (\d+)/)))
    state.dims=[+m[1],+m[2],+m[3]];
  if((m=l.match(/extended world height: min_y (-?\d+), height (\d+)/)))
    state.height={min:+m[1],h:+m[2]};
  if((m=l.match(/occupied = (\d+)/))) state.occupied=+m[1];
  if((m=l.match(/filled (~?)([\d]+) pinholes, removed (~?)([\d]+)/)))
    state.clean={fill:+m[2],crumb:+m[4]};
  if((m=l.match(/tile (\d+): (\d+) voxels, (\d+) chunks/)))
    state.tiles.push({i:+m[1],v:+m[2],c:+m[3]});
  if((m=l.match(/wrote .*\/(r\.-?\d+\.-?\d+\.mca)/))) state.regions.push(m[1]);
  if(/FORGE COMPLETE/.test(l)) state.complete=true;
}

// ---------------- three.js stage -----------------------------------------
function initThree(){
  const cv=$('#sc-gl');
  const r=new THREE.WebGLRenderer({canvas:cv,antialias:true});
  r.setClearColor(0x05070a,1);
  r.setSize(innerWidth,innerHeight); r.setPixelRatio(Math.min(devicePixelRatio,2));
  const scene=new THREE.Scene();
  scene.fog=new THREE.FogExp2(0x05070a,0.10);
  const cam=new THREE.PerspectiveCamera(50,innerWidth/innerHeight,0.01,100);
  cam.position.set(2.6,1.4,2.6);
  scene.add(new THREE.AmbientLight(0xffffff,0.75));
  const key=new THREE.DirectionalLight(0xcfe4ff,1.1); key.position.set(3,5,2); scene.add(key);
  const rim=new THREE.DirectionalLight(0x38c8ff,0.5); rim.position.set(-4,2,-3); scene.add(rim);
  const grid=new THREE.GridHelper(6,30,0x1c3a52,0x14232f);
  grid.position.y=-1.05; scene.add(grid);
  three={r,scene,cam,grid};
  addEventListener('resize',()=>{r.setSize(innerWidth,innerHeight);
    cam.aspect=innerWidth/innerHeight; cam.updateProjectionMatrix();});
}

function buildPoints(pts){
  const g=new THREE.BufferGeometry();
  g.setAttribute('position',new THREE.Float32BufferAttribute(pts,3));
  const m=new THREE.PointsMaterial({color:0x38c8ff,size:0.012,transparent:true,opacity:0.9});
  const p=new THREE.Points(g,m); p.geometry.setDrawRange(0,0);
  three.scene.add(p); return p;
}

function buildVoxels(data){
  // normalise world coords into the same [-1,1] frame as the point cloud
  let lo=[1e9,1e9,1e9],hi=[-1e9,-1e9,-1e9];
  for(const c of data.chunks)for(const v of c.v)for(let k=0;k<3;k++){
    lo[k]=Math.min(lo[k],v[k]); hi[k]=Math.max(hi[k],v[k]);}
  const ext=Math.max(hi[0]-lo[0],hi[1]-lo[1],hi[2]-lo[2],1);
  const s=2/ext, cx=(hi[0]+lo[0])/2, cy=(hi[1]+lo[1])/2, cz=(hi[2]+lo[2])/2;
  const n=data.sampled;
  const geo=new THREE.BoxGeometry(s*Math.max(1,data.stride**(1/3))*0.95,
                                  s*Math.max(1,data.stride**(1/3))*0.95,
                                  s*Math.max(1,data.stride**(1/3))*0.95);
  const mat=new THREE.MeshLambertMaterial();
  const im=new THREE.InstancedMesh(geo,mat,n);
  im.instanceColor=new THREE.InstancedBufferAttribute(new Float32Array(n*3),3);
  const M=new THREE.Matrix4();
  const order=[]; let i=0;
  for(const c of data.chunks){
    const hue=((c.cx*73856093 ^ c.cz*19349663)>>>0)%360;
    const cc=new THREE.Color().setHSL(hue/360,0.85,0.55);
    for(const v of c.v){
      M.makeTranslation((v[0]-cx)*s,(v[1]-cy)*s,(v[2]-cz)*s);
      im.setMatrixAt(i,M);
      const p=data.palette[v[3]];
      order.push({i,chunkCol:cc,blockCol:new THREE.Color(p[1]/255,p[2]/255,p[3]/255)});
      im.setColorAt(i,cc); i++;
    }
  }
  im.count=0; three.scene.add(im);
  return {im,order,chunkStarts:chunkStarts(data),data};
}
function chunkStarts(data){
  const st=[]; let i=0;
  for(const c of data.chunks){st.push({at:i,len:c.v.length,cx:c.cx,cz:c.cz}); i+=c.v.length;}
  return st;
}

// ---------------- 2D overlay helpers -------------------------------------
function ctx2d(id){const c=$(id); c.width=c.clientWidth*2; c.height=c.clientHeight*2;
  const x=c.getContext('2d'); x.scale(2,2); return x;}

// ---------------- the show ------------------------------------------------
async function start(jobId,{duration=180,live=true}={}){
  const stage=$('#stage'); stage.classList.add('on');
  $('#sc-skip').onclick=()=>finish(true);
  document.onkeydown=e=>{if(e.key==='Escape')finish(true);};

  state={jobId,t0:performance.now(),duration,phase:0,phaseT0:performance.now(),
         lines:[],tiles:[],regions:[],complete:false,mesh:null,calib:null,
         dims:null,height:null,occupied:null,clean:null,
         points:null,vox:null,voxReq:false,meshReq:false,done:false,
         install:$('#sc-install'),palette:null,swatchDone:false};

  // phase HUD
  const hud=$('#sc-phases'); hud.innerHTML='';
  state.phEls=PH.map(([k,label])=>{
    const d=el('div','sc-ph',hud); d.innerHTML=
      `<span class="k">${label}</span><span class="bar"><span></span></span>`;
    return d;});

  initThree();

  // data feeds ------------------------------------------------------------
  fetch('/api/palette').then(r=>r.json()).then(p=>state.palette=p);
  if(live){
    window.jobFeed?.subscribe?.(l=>{state.lines.push(l);parseLine(state,l);});
  } else {
    const j=await (await fetch('/api/loglines/'+jobId)).json();
    j.lines.forEach((l,ix)=>setTimeout(()=>{state.lines.push(l);parseLine(state,l);},ix*120));
    setTimeout(()=>{if(j.status==='done')state.complete=true;},j.lines.length*120+150);
  }
  requestAnimationFrame(tick);
}

function phaseDur(i){return PH[i][2]*state.duration*1000;}

function setPhase(i){
  state.phase=i; state.phaseT0=performance.now();
  state.phEls.forEach((e,ix)=>e.classList.toggle('on',ix===i));
  state.phEls.forEach((e,ix)=>e.classList.toggle('past',ix<i));
  $('#sc-title').textContent=PH[i][1];
  ticker(`>> phase ${i+1}/${PH.length} :: ${PH[i][1]}`);
}

let tickerLines=[];
function ticker(t){
  tickerLines.push(t); if(tickerLines.length>7)tickerLines.shift();
  $('#sc-ticker').innerHTML=tickerLines.map((l,i)=>
    `<div style="opacity:${0.35+0.65*(i/6)}">${l}</div>`).join('');
}

function counter(id,val,fmt){$(id).textContent=fmt?fmt(val):val.toLocaleString();}

function tick(){
  if(!state||state.done)return;
  const now=performance.now();
  const pt=Math.min(1,(now-state.phaseT0)/phaseDur(state.phase));
  const bar=state.phEls[state.phase].querySelector('.bar span');
  bar.style.width=(pt*100)+'%';

  const P=PH[state.phase][0];
  const holdReasons={mesh:()=>state.points, voxel:()=>state.vox&&state.complete!==null,
                     done:()=>true};
  // ---- per-phase update ----
  if(P==='init')  phInit(pt);
  if(P==='mesh')  phMesh(pt);
  if(P==='calib') phCalib(pt);
  if(P==='pal')   phPal(pt);
  if(P==='voxel') phVoxel(pt,now);
  if(P==='clean') phClean(pt);
  if(P==='asm')   phAsm(pt);
  if(P==='done')  phDone(pt);

  // camera orbit
  const t=(now-state.t0)/1000;
  const rad=2.9-0.35*Math.sin(t*0.13), h=1.15+0.5*Math.sin(t*0.09);
  three.cam.position.set(Math.cos(t*0.16)*rad,h,Math.sin(t*0.16)*rad);
  three.cam.lookAt(0,-0.05,0);
  three.r.render(three.scene,three.cam);
  if(window._shots){
    const el=(now-state.t0)/1000;
    for(const mark of window._shots){
      if(!mark.done&&el>=mark.t){mark.done=true;
        try{fetch('/api/debugshot?t='+mark.t,{method:'POST',
          body:$('#sc-gl').toDataURL('image/png')});}catch(e){}
        fetch('/api/debuglog',{method:'POST',
          body:`t=${mark.t} phase=${PH[state.phase][0]} pts=${!!state.points} vox=${state.vox?state.vox.im.count:'-'} lines=${state.lines.length} regions=${state.regions.length} complete=${state.complete} voxReq=${state.voxReq}`});
      }
    }
  }

  // advance
  if(pt>=1){
    const gate={mesh:()=>!!state.points||state.meshFail,
                voxel:()=>!!state.vox}[P];
    if(!gate||gate()){
      if(state.phase<PH.length-1)setPhase(state.phase+1);
      else return finish(false);
    } else if(!state._waitNote||now-state._waitNote>3000){
      ticker('… waiting on pipeline data'); state._waitNote=now;
    }
  }
  requestAnimationFrame(tick);
}

// ---- phases --------------------------------------------------------------
function phInit(pt){
  if(!state._init){state._init=1; setPhase(0);
    ticker('boot :: objtomc engine (GPU voxelizer · Oklab matcher · anvil writer)');
    fetch('/api/mesh/'+state.jobId).then(r=>r.json())
      .then(d=>{state.meshData=d; state.points=buildPoints(d.points);})
      .catch(()=>state.meshFail=true);
  }
  if(state.mesh&&!state._initMesh){state._initMesh=1;
    ticker(`target locked :: ${state.mesh.pos.toLocaleString()} vertices / ${state.mesh.faces.toLocaleString()} triangles / ${state.mesh.mats} materials`);}
}
function phMesh(pt){
  if(state.points){
    const total=state.points.geometry.attributes.position.count;
    state.points.geometry.setDrawRange(0,Math.floor(total*pt));
    state.points.rotation.y+=0.0022;
    if(state.mesh) counter('#sc-n1',Math.floor(state.mesh.pos*pt));
  }
}
function phCalib(pt){
  const c=$('#sc-diag'); c.classList.add('on');
  const x=state._dx||(state._dx=ctx2d('#sc-diag-cv'));
  const W=c.clientWidth-24,H=c.clientHeight-40;
  x.clearRect(0,0,W+24,H+40);
  x.strokeStyle='#4da3e0'; x.fillStyle='#a9c4da'; x.font='11px monospace'; x.lineWidth=1;
  const ab=state.meshData?state.meshData.aabb:[1,1,1];
  const m=Math.max(...ab); const w=120*(ab[0]/m),h=80*(ab[1]/m),d=60*(ab[2]/m);
  const ox=40,oy=H-30;
  const p=pt;
  x.globalAlpha=Math.min(1,p*3);
  // iso box
  x.strokeRect(ox,oy-h,w,h);
  x.beginPath(); x.moveTo(ox,oy-h); x.lineTo(ox+d*0.6,oy-h-d*0.5);
  x.lineTo(ox+w+d*0.6,oy-h-d*0.5); x.lineTo(ox+w,oy-h); x.stroke();
  x.beginPath(); x.moveTo(ox+w,oy); x.lineTo(ox+w+d*0.6,oy-d*0.5);
  x.lineTo(ox+w+d*0.6,oy-h-d*0.5); x.stroke();
  // measure animation on longest axis
  x.strokeStyle='#38c8ff';
  x.beginPath(); x.moveTo(ox,oy+12); x.lineTo(ox+w*Math.min(1,p*2),oy+12); x.stroke();
  x.fillText(`AABB ${ab.map(v=>v.toFixed(1)).join(' × ')}`,ox,20);
  if(state.calib&&p>0.4){
    x.fillText(`voxel_size = ${ab[0]>=Math.max(ab[1],ab[2])?ab[0].toFixed(2):m.toFixed(2)} / ${state.calib.target} = ${state.calib.vs}`,ox,36);
  }
  if(state.dims&&p>0.6) x.fillText(`grid ${state.dims.join(' × ')}`,ox,52);
  if(state.height&&p>0.8){x.fillStyle='#d8a04a';
    x.fillText(`⚑ EXTENDED HEIGHT ${state.height.h} (datapack embed)`,ox,68);}
  x.globalAlpha=1;
}
function phPal(pt){
  $('#sc-diag').classList.remove('on');
  const c=$('#sc-pal'); c.classList.add('on');
  if(!state.palette)return;
  const x=state._px||(state._px=ctx2d('#sc-pal-cv'));
  const W=c.clientWidth-24,H=c.clientHeight-44;
  const n=state.palette.length, upto=Math.floor(n*Math.min(1,pt*1.6));
  const cols=Math.ceil(Math.sqrt(n*W/H)), cw=W/cols;
  x.clearRect(0,0,W+24,H+44);
  for(let i=0;i<upto;i++){
    const p=state.palette[i];
    x.fillStyle=`rgb(${p[1]},${p[2]},${p[3]})`;
    x.fillRect(12+(i%cols)*cw,20+Math.floor(i/cols)*cw,cw-1,cw-1);
  }
  x.fillStyle='#a9c4da'; x.font='11px monospace';
  x.fillText(`block palette :: ${upto}/${n} · Oklab nearest-neighbour match`,12,12);
  // live oklab match demo (real formula)
  if(pt>0.5&&(!state._lastMatch||performance.now()-state._lastMatch>700)){
    state._lastMatch=performance.now();
    const r=Math.random(),g=Math.random(),b=Math.random();
    const t=oklab(r,g,b); let bi=0,bd=1e9;
    for(let i=0;i<n;i++){const p=state.palette[i];
      const o=oklab(p[1]/255,p[2]/255,p[3]/255);
      const d=(o[0]-t[0])**2+(o[1]-t[1])**2+(o[2]-t[2])**2;
      if(d<bd){bd=d;bi=i;}}
    ticker(`match rgb(${(r*255)|0},${(g*255)|0},${(b*255)|0}) → ${state.palette[bi][0]} (ΔOklab ${Math.sqrt(bd).toFixed(4)})`);
  }
}
function phVoxel(pt,now){
  $('#sc-pal').classList.remove('on');
  if(!state.voxReq&&state.complete){state.voxReq=true;
    fetch('/api/voxels/'+state.jobId).then(r=>r.json())
      .then(d=>{try{state.voxData=d; state.vox=buildVoxels(d);
        ticker(`voxel field :: ${d.total.toLocaleString()} voxels / ${d.chunks.length} chunks`);}
        catch(e){fetch('/api/debuglog',{method:'POST',body:'BUILDVOX ERR '+e.message+' | '+e.stack});}})
      .catch(e=>fetch('/api/debuglog',{method:'POST',body:'VOXFETCH ERR '+e.message}));}
  if(state.points){ // dissolve the point cloud
    state.points.material.opacity=Math.max(0,0.9-pt*1.4);
  }
  if(state.vox){
    const n=state.vox.order.length;
    state.vox.im.count=Math.floor(n*pt);
    state.vox.im.instanceMatrix.needsUpdate=true;
    if(state.vox.im.instanceColor)state.vox.im.instanceColor.needsUpdate=true;
    // chunk ticker
    const cs=state.vox.chunkStarts;
    const ci=cs.findIndex(c=>c.at<=n*pt&&n*pt<c.at+c.len);
    if(ci>=0&&ci!==state._lastChunk){state._lastChunk=ci;
      counter('#sc-n2',Math.floor(n*pt));
      if(ci%3===0)ticker(`chunk (${cs[ci].cx}, ${cs[ci].cz}) committed`);}
  } else if(state.tiles.length&&state.tiles.length!==state._lastTile){
    state._lastTile=state.tiles.length;
    const t=state.tiles[state.tiles.length-1];
    ticker(`tile ${t.i}: ${t.v.toLocaleString()} voxels → ${t.c} chunks`);
  }
}
function phClean(pt){
  if(state.vox){ // colour transition chunk-hue -> real block colours + pop
    const n=state.vox.order.length, upto=Math.floor(n*pt);
    for(let i=state._recol||0;i<upto;i++){
      const o=state.vox.order[i];
      state.vox.im.setColorAt(o.i,o.blockCol);
    }
    state._recol=upto;
    state.vox.im.instanceColor.needsUpdate=true;
  }
  if(state.clean&&!state._cleanT){state._cleanT=1;
    ticker(`refine :: ${state.clean.fill.toLocaleString()} pinholes sealed · ${state.clean.crumb.toLocaleString()} crumbs removed`);}
  counter('#sc-n3',state.clean?Math.floor(state.clean.fill*Math.min(1,pt*1.3)):0);
}
function phAsm(pt){
  const c=$('#sc-reg'); c.classList.add('on');
  const x=state._rx||(state._rx=ctx2d('#sc-reg-cv'));
  const W=c.clientWidth-24,H=c.clientHeight-44;
  const regs=state.regions.map(rn=>{const m=rn.match(/r\.(-?\d+)\.(-?\d+)/);
    return {x:+m[1],z:+m[2],n:rn};});
  x.clearRect(0,0,W+24,H+44);
  x.fillStyle='#a9c4da'; x.font='11px monospace';
  x.fillText(`anvil regions :: ${regs.length} file(s) · 512×512 each`,12,12);
  if(regs.length){
    const xs=regs.map(r=>r.x),zs=regs.map(r=>r.z);
    const x0=Math.min(...xs),z0=Math.min(...zs);
    const nx=Math.max(...xs)-x0+1,nz=Math.max(...zs)-z0+1;
    const cell=Math.min((W-20)/nx,(H-20)/nz,44);
    const upto=Math.ceil(regs.length*Math.min(1,pt*1.5));
    regs.slice(0,upto).forEach((r,i)=>{
      const gx=16+(r.x-x0)*cell, gz=24+(r.z-z0)*cell;
      x.fillStyle=i===upto-1?'#4da3e0':'#24405a';
      x.fillRect(gx,gz,cell-3,cell-3);
      x.fillStyle='#0b1218'; x.font='9px monospace';
      x.fillText(r.n.replace('.mca',''),gx+3,gz+cell/2);
    });
  }
  if(state.vox)state.vox.im.rotation.y+=0.0012;
}
function phDone(pt){
  $('#sc-reg').classList.remove('on');
  if(!state._doneT){state._doneT=1;
    const st=[];
    if(state.occupied)st.push(`${state.occupied.toLocaleString()} voxels`);
    if(state.dims)st.push(`grid ${state.dims.join('×')}`);
    if(state.regions.length)st.push(`${state.regions.length} regions`);
    if(state.height)st.push(`height ${state.height.h}`);
    $('#sc-final').innerHTML=`<div class="big">WORLD FORGED</div>
      <div class="sub">${st.join(' · ')}</div>`;
    $('#sc-final').classList.add('on');
    state.install.classList.add('on');
    ticker(':: level.dat written · world vanilla-loadable');
  }
  if(state.vox)state.vox.im.rotation.y+=0.002;
}

function finish(skipped){
  if(!state)return;
  state.done=true;
  if(skipped)ticker(':: showcase skipped');
  setTimeout(()=>{ $('#stage').classList.remove('on');
    ['#sc-diag','#sc-pal','#sc-reg','#sc-final'].forEach(s=>$(s).classList.remove('on'));
    state.install.classList.remove('on');
    if(three.r){three.r.dispose?.(); $('#sc-gl').getContext('webgl2')?.getExtension('WEBGL_lose_context')?.loseContext?.();}
    state=null;
  }, skipped?50:2600);
}

return {start};
})();
