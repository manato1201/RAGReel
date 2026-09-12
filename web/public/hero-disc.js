// Real WebGL chrome disc for the landing hero (index.html). Loaded as a
// classic (non-module) script against a globally-exposed `THREE` -- this
// dashboard is opened via file:// by the desktop app (VideoHistory::
// openDashboard(), engine/src/launcher/video_history.cpp), and ES module
// imports are blocked by the browser from a file:// document (CORS), so
// this intentionally avoids import/export and any addon files that would
// only ship as ES modules. If the CDN <script> tag before this one fails
// to load (no network), THREE is undefined and we bail out, leaving
// #heroFallback (plain CSS, in the DOM already) visible.
//
// Site palette only (styles.css tokens, hex-literal here since this runs
// outside CSS): --bg #f6f3ec, --panel #ffffff, --text-0 #1c1c1c,
// --text-1 #3c3c3c, --orange #ff7a45, --mint #35c98f.

(function () {
  if (typeof THREE === "undefined") return;

  var COLORS = {
    bg: 0xf6f3ec,
    panel: 0xffffff,
    floor: 0x3c3c3c,
    face: 0xf4f2ec,
    edge: 0xcac6b8,
    hub: 0x1c1c1c,
    orange: 0xff7a45,
    mint: 0x35c98f,
  };

  // A small procedural "room" fed into PMREMGenerator so the metal
  // material has something to reflect (a pure-metal PBR material with no
  // environment renders almost black). Same idea as three/examples'
  // RoomEnvironment.js, written inline instead of importing it, since
  // that addon only ships as an ES module.
  function buildRoomEnv() {
    var room = new THREE.Scene();
    var geo = new THREE.PlaneGeometry(20, 20);

    function wall(color, x, y, z, rx, ry) {
      var mesh = new THREE.Mesh(geo, new THREE.MeshBasicMaterial({ color: color }));
      mesh.position.set(x, y, z);
      mesh.rotation.set(rx, ry, 0);
      room.add(mesh);
    }

    // Dim, desaturated versions of the site colors -- these act as unlit
    // "light sources" once baked into the PMREM environment, so a wall at
    // full brightness (e.g. pure white or the site's saturated --orange)
    // blows the chrome reflections out to near-white. Kept dark enough
    // that the metal reads as metal (bright highlights against visible
    // mid-tones), not a blank glowing sheet.
    wall(0x8a8778, 0, 8, 0, Math.PI / 2, 0); // ceiling
    wall(0x2a2a2a, 0, -8, 0, -Math.PI / 2, 0); // floor
    wall(0x6b3a22, -8, 0, 0, 0, Math.PI / 2); // left wall, dim orange
    wall(0x1d5f43, 8, 0, 0, 0, -Math.PI / 2); // right wall, dim mint
    wall(0x3a382f, 0, 0, -8, 0, 0); // back wall
    wall(0x6f6c60, 0, 0, 8, 0, Math.PI); // front wall

    return room;
  }

  function makeGlowTexture() {
    var size = 128;
    var canvas = document.createElement("canvas");
    canvas.width = canvas.height = size;
    var ctx = canvas.getContext("2d");
    var g = ctx.createRadialGradient(size / 2, size / 2, 0, size / 2, size / 2, size / 2);
    g.addColorStop(0, "rgba(255,255,255,1)");
    g.addColorStop(0.4, "rgba(255,255,255,0.55)");
    g.addColorStop(1, "rgba(255,255,255,0)");
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, size, size);
    return new THREE.CanvasTexture(canvas);
  }

  // Faint concentric rainbow rings baked onto the disc face, like the
  // diffraction sheen a real DVD's data layer catches at an angle. Low
  // alpha so it reads as a hint of iridescence riding on top of the chrome
  // reflections, not a literal rainbow -- same allowance the reference
  // style doc makes for its own rendered asset ("the only color belongs to
  // the rendered scene", separate from the site's own UI token palette).
  function makeDiscSheenTexture() {
    var size = 512;
    var canvas = document.createElement("canvas");
    canvas.width = canvas.height = size;
    var ctx = canvas.getContext("2d");
    // On a metalness:1 material this texture tints the specular
    // reflection (metals have no diffuse term), so the untouched
    // background must be neutral white (full pass-through), not
    // transparent -- a transparent/black background here would tint the
    // whole reflected environment toward black wherever no ring is drawn.
    ctx.fillStyle = "#ffffff";
    ctx.fillRect(0, 0, size, size);
    var cx = size / 2;
    var cy = size / 2;
    var hues = [18, 190, 150, 265];
    for (var r = size * 0.12; r < size * 0.49; r += 2.2) {
      var hue = hues[Math.floor(r / 2.2) % hues.length];
      ctx.strokeStyle = "hsla(" + hue + ", 75%, 70%, 0.06)";
      ctx.lineWidth = 1.3;
      ctx.beginPath();
      ctx.arc(cx, cy, r, 0, Math.PI * 2);
      ctx.stroke();
    }
    return new THREE.CanvasTexture(canvas);
  }

  // A true washer/annulus solid -- a circular Shape with a circular hole,
  // extruded to real thickness -- instead of the previous cylinder + a
  // separate dark "hub" disc covering the center. This is what makes the
  // hole an actual hole (you see through it to whatever is behind the
  // disc) rather than a painted-on circle, and it needs no axis
  // reorientation: the shape already lies in XY with the extrusion along
  // Z, so its flat faces point at the camera by construction.
  function buildDvdGeometry(outerRadius, innerRadius, depth) {
    var shape = new THREE.Shape();
    shape.absarc(0, 0, outerRadius, 0, Math.PI * 2, false);
    var hole = new THREE.Path();
    hole.absarc(0, 0, innerRadius, 0, Math.PI * 2, true);
    shape.holes.push(hole);

    var geo = new THREE.ExtrudeGeometry(shape, {
      depth: depth,
      bevelEnabled: false,
      curveSegments: 128,
    });
    geo.center();
    return geo;
  }

  function makeSegmentMesh(color) {
    return new THREE.Mesh(
      new THREE.BoxGeometry(0.022, 1, 0.022),
      new THREE.MeshBasicMaterial({
        color: color,
        transparent: true,
        opacity: 0.5,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
      })
    );
  }

  // A box's long axis is local Y by default -- align it to a-to-b directly
  // instead of lookAt()-ing a plane, which only orients a face normal and
  // goes invisible edge-on from some angles. scale.y stretches the unit-
  // length box rather than rebuilding geometry every frame.
  function positionSegment(mesh, a, b) {
    var dir = new THREE.Vector3().subVectors(b, a);
    var len = dir.length();
    mesh.position.copy(a).addScaledVector(dir, 0.5);
    mesh.scale.set(1, len, 1);
    mesh.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir.normalize());
  }

  // A laser as two segments meeting at a fixed point near the disc: a
  // static incoming ray, and an outgoing ray whose direction is
  // recomputed every frame from the disc's *current* face normal
  // (updateReflection, called from the render loop) via Vector3.reflect()
  // -- so it visibly bounces off the spinning disc instead of passing
  // straight through it. depthTest stays on (the default), so either leg
  // is correctly hidden behind the opaque disc where it's occluded.
  function makeBeam(scene, color, from, hitPoint, reach) {
    var incidentDir = new THREE.Vector3().subVectors(hitPoint, from).normalize();

    var incomingMesh = makeSegmentMesh(color);
    positionSegment(incomingMesh, from, hitPoint);
    scene.add(incomingMesh);

    var outgoingMesh = makeSegmentMesh(color);
    scene.add(outgoingMesh);

    // A glow sprite plus a real point light at the hit point, both pulsed
    // in the render loop, so the chrome disc picks up an actual colored
    // specular highlight where the laser strikes it, not just a flat
    // sprite sitting in front of it.
    var glow = new THREE.Sprite(
      new THREE.SpriteMaterial({ map: makeGlowTexture(), color: color, transparent: true, depthWrite: false, blending: THREE.AdditiveBlending })
    );
    glow.position.copy(hitPoint);
    glow.scale.set(0.38, 0.38, 1);
    scene.add(glow);

    var light = new THREE.PointLight(color, 0, 3.6, 2);
    light.position.copy(hitPoint);
    scene.add(light);

    return {
      incomingMesh: incomingMesh,
      outgoingMesh: outgoingMesh,
      glow: glow,
      light: light,
      updateReflection: function (discNormal) {
        var reflectedDir = incidentDir.clone().reflect(discNormal);
        var end = hitPoint.clone().addScaledVector(reflectedDir, reach);
        positionSegment(outgoingMesh, hitPoint, end);
      },
    };
  }

  function initHeroDisc() {
    var mount = document.getElementById("heroScene");
    var fallback = document.getElementById("heroFallback");
    if (!mount) return;

    var width = mount.clientWidth;
    var height = mount.clientHeight;
    if (!width || !height) return;

    var renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    renderer.setSize(width, height);
    if ("outputColorSpace" in renderer) {
      renderer.outputColorSpace = THREE.SRGBColorSpace;
    } else {
      renderer.outputEncoding = THREE.sRGBEncoding;
    }
    renderer.toneMapping = THREE.ACESFilmicToneMapping;
    renderer.toneMappingExposure = 0.85;
    renderer.domElement.style.position = "absolute";
    renderer.domElement.style.inset = "0";
    mount.appendChild(renderer.domElement);

    var scene = new THREE.Scene();
    var camera = new THREE.PerspectiveCamera(30, width / height, 0.1, 100);
    camera.position.set(0, 0.7, 6.2);
    camera.lookAt(0, 0, 0);

    var pmrem = new THREE.PMREMGenerator(renderer);
    scene.environment = pmrem.fromScene(buildRoomEnv(), 0.04).texture;

    // rig bobs up/down as one unit; the continuous spin (rotateOnWorldAxis,
    // applied to the quaternion directly every frame) is independent of
    // this static rig tilt, so the two never fight each other the way
    // stacked Euler-angle fields on the same object did before.
    var rig = new THREE.Group();
    scene.add(rig);

    var faceMat = new THREE.MeshStandardMaterial({
      color: COLORS.face,
      metalness: 1,
      roughness: 0.34,
      map: makeDiscSheenTexture(),
    });
    var edgeMat = new THREE.MeshStandardMaterial({ color: COLORS.edge, metalness: 1, roughness: 0.48 });
    var disc = new THREE.Mesh(buildDvdGeometry(1.55, 0.48, 0.06), [faceMat, edgeMat]);
    // A single, deliberate tilt (steep pitch + a diagonal yaw) instead of
    // the previous 3-axis reorientation -- the geometry already faces the
    // camera by construction, so rotation.x/.y here are a pure "lean the
    // disc back and turn it" pose, nothing is fighting a hidden axis swap.
    disc.rotation.x = 1.05;
    disc.rotation.y = 0.55;
    rig.add(disc);

    // Hit points sit on the solid metal ring (disc hole radius is 0.48,
    // outer radius 1.55 -- these are ~0.85 out, comfortably on the metal,
    // not inside the hole). reach is how far the reflected leg travels
    // past the hit point (long enough to exit the frame in any direction
    // it ends up pointing as the disc spins).
    var beamA = makeBeam(scene, COLORS.orange, new THREE.Vector3(-3.6, 2.4, 1.4), new THREE.Vector3(0.75, 0.42, 0.1), 4);
    var beamB = makeBeam(scene, COLORS.mint, new THREE.Vector3(3.6, 2.2, -1.2), new THREE.Vector3(-0.7, -0.28, 0.1), 4);

    scene.add(new THREE.AmbientLight(0xffffff, 0.06));

    fallback.style.display = "none";

    var clock = new THREE.Clock();
    var stopped = false;
    var worldY = new THREE.Vector3(0, 1, 0);
    var discNormal = new THREE.Vector3();
    var discFaceNormalLocal = new THREE.Vector3(0, 0, 1);

    function tick() {
      if (stopped) return;
      var t = clock.getElapsedTime();

      disc.rotateOnWorldAxis(worldY, 0.006);
      rig.position.y = Math.sin(t * 0.9) * 0.18;

      discNormal.copy(discFaceNormalLocal).applyQuaternion(disc.quaternion).normalize();
      beamA.updateReflection(discNormal);
      beamB.updateReflection(discNormal);

      var pulseA = (Math.sin(t * 1.9) + 1) / 2;
      var pulseB = (Math.sin(t * 1.9 + Math.PI) + 1) / 2;
      beamA.incomingMesh.material.opacity = beamA.outgoingMesh.material.opacity = 0.22 + pulseA * 0.42;
      beamA.glow.material.opacity = 0.12 + pulseA * 0.55;
      beamA.light.intensity = pulseA * 1.6;
      beamB.incomingMesh.material.opacity = beamB.outgoingMesh.material.opacity = 0.22 + pulseB * 0.42;
      beamB.glow.material.opacity = 0.12 + pulseB * 0.55;
      beamB.light.intensity = pulseB * 1.6;

      renderer.render(scene, camera);
      requestAnimationFrame(tick);
    }
    tick();

    window.addEventListener("resize", function () {
      var w = mount.clientWidth;
      var h = mount.clientHeight;
      if (!w || !h) return;
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      renderer.setSize(w, h);
    });

    document.addEventListener("visibilitychange", function () {
      stopped = document.hidden;
      if (!stopped) tick();
    });
  }

  try {
    initHeroDisc();
  } catch (err) {
    console.warn("Hero disc: WebGL unavailable, using static fallback.", err);
  }
})();
