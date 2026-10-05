849:  <script>
850-    // ─── State ─────────────────────────────────────────────────────────────────
851-
852-    let ws = null;
853-    let ctrlData = {
854-      steering_angle: 0,
855-      steering_direction: 'CENTER',
856-      steering_raw: 0,
857-      throttle: 0,
858-      brake: 0,
859-      buttons: {},
860-      connected: false,
861-      timestamp: 0
862-    };
863-
864-    let lastTs = 0, pktCount = 0, fpsCalc = 0, lastFpsTime = Date.now();
865-    let hzHistory = [], hzInterval = null;
866-    let currentViewSteer = 0; // Default view Setir 1
867-
868-    function setViewSteer(idx) {
869-      currentViewSteer = idx;
870-      for(let i=0; i<3; i++) {
871-        let btn = document.getElementById('btn-view-' + i);
872-        if(!btn) continue;
873-        if(i === idx) {
874-          btn.style.borderColor = 'var(--accent)';
875-          btn.style.color = 'var(--bg)';
876-          btn.style.background = 'var(--accent)';
877-        } else {
878-          btn.style.borderColor = 'var(--muted)';
879-          btn.style.color = 'var(--muted)';
880-          btn.style.background = 'transparent';
881-        }
882-      }
883-    }
884-
885-    // ─── WebSocket ──────────────────────────────────────────────────────────────
886-
887-    function toggleWS() {
888-      if (ws && ws.readyState === WebSocket.OPEN) {
889-        ws.close();
890-        return;
891-      }
892-      const url = document.getElementById('ws-url').value.trim();
893-      connectWS(url);
894-    }
895-
896-    function connectWS(url) {
897-      try {
898-        ws = new WebSocket(url);
899-
900-        ws.onopen = () => {
901-          setDot('ws', true, 'WS: OK');
902-          document.getElementById('btn-conn').textContent = 'DISCONNECT';
903-          addLog('SYS', 'Terhubung ke ' + url);
904-          ws.send(JSON.stringify({ type: 'get_config' }));
905-        };
906-
907-        ws.onclose = () => {
908-          setDot('ws', false, 'WS: OFF');
909-          setDot('ctrl', false, 'CTRL: OFF');
910-          document.getElementById('btn-conn').textContent = 'CONNECT';
911-          addLog('SYS', 'Koneksi terputus');
912-          setTimeout(() => connectWS(url), 3000);
913-        };
914-
915-        ws.onerror = (e) => {
916-          addLog('ERR', 'WebSocket error');
917-        };
918-
919-        ws.onmessage = (evt) => {
920-          const msg = JSON.parse(evt.data);
921-          handleMessage(msg);
922-        };
923-      } catch (e) {
924-        addLog('ERR', e.message);
925-      }
926-    }
927-
928-    function handleMessage(msg) {
929-      if (msg.type === 'controller_data') {
930-        // Filter based on selected steer view
931-        if (msg.steer_index !== undefined && msg.steer_index !== currentViewSteer) {
932-          return;
933-        }
934-
935-        const now = Date.now();
936-        const lat = lastTs ? (now - lastTs) : 0;
937-        lastTs = now;
938-        pktCount++;
939-
940-        ctrlData = msg.data;
941-
942-        hzHistory.push(now);
943-        hzHistory = hzHistory.filter(t => now - t < 1000);
944-        document.getElementById('hz-val').textContent = hzHistory.length;
945-        document.getElementById('info-lat').textContent = lat + ' ms';
946-        document.getElementById('info-pkt').textContent = pktCount;
947-        document.getElementById('info-ts').textContent = new Date(ctrlData.timestamp * 1000).toLocaleTimeString();
948-        
949-        if (ctrlData.active_car_id !== undefined) {
950-          document.getElementById('car-id-val').textContent = ctrlData.active_car_id;
951-        }
952-
953-        setDot('ctrl', ctrlData.connected, 'CTRL: ' + (ctrlData.connected ? 'ON' : 'OFF'));
954-        updateUI();
955-      } else if (msg.type === 'config') {
956-        applyConfig(msg.data);
957-      } else if (msg.type === 'config_updated') {
958-        addLog('CAL', 'Kalibrasi tersimpan');
959-      }
960-    }
961-
962-    // ─── UI Updates ─────────────────────────────────────────────────────────────
963-
964-    function updateUI() {
965-      const d = ctrlData;
966-
967-      // Steering angle
968-      document.getElementById('angle-display').textContent = d.steering_angle.toFixed(1) + '°';
969-
970-      // Direction badge
971-      const badge = document.getElementById('dir-badge');
972-      badge.textContent = d.steering_direction;
973-      badge.className = 'direction-badge dir-' + d.steering_direction.toLowerCase();
974-
975-      // Pedal bars horizontal
976-      const tPct = Math.round(d.throttle * 100);
977-      const bPct = Math.round(d.brake * 100);
978-      document.getElementById('throttle-bar').style.width = tPct + '%';
979-      document.getElementById('brake-bar').style.width = bPct + '%';
980-      document.getElementById('throttle-big').textContent = tPct + '%';
981-      document.getElementById('brake-big').textContent = bPct + '%';
982-
983-      // Vertical bars
984-      document.getElementById('throttle-vbar').style.height = tPct + '%';
985-      document.getElementById('brake-vbar').style.height = bPct + '%';
986-      document.getElementById('throttle-pct').textContent = tPct + '%';
987-      document.getElementById('brake-pct').textContent = bPct + '%';
988-
989-      // Axis thumbs (map -1..1 to 0..100%)
990-      setAxisThumb('ax-steer', d.steering_raw, d.steering_raw.toFixed(4));
991-      setAxisThumb('ax-thr', d.throttle * 2 - 1, d.throttle.toFixed(3));
992-      setAxisThumb('ax-brk', d.brake * 2 - 1, d.brake.toFixed(3));
993-
994-      // Buttons
995-      updateButtons(d.buttons);
996-
997-      // Draw wheel
998-      drawWheel(d.steering_angle);
999-
1000-      // ESP log (only significant changes)
1001-      if (Math.abs(d.steering_angle) > 1 || d.throttle > 0.01 || d.brake > 0.01) {
1002-        if (pktCount % 10 === 0) {
1003-          addLog('RC', `STR:${d.steering_angle.toFixed(1)}° GAS:${tPct}% REM:${bPct}%`);
1004-        }
1005-      }
1006-    }
1007-
1008-    function setAxisThumb(id, val, label) {
1009-      const pct = ((val + 1) / 2 * 100).toFixed(1);
1010-      const el = document.getElementById(id + '-thumb');
1011-      if (el) el.style.left = pct + '%';
1012-      const vEl = document.getElementById(id + '-val');
1013-      if (vEl) vEl.textContent = label;
1014-    }
1015-
1016-    function updateButtons(btns) {
1017-      const grid = document.getElementById('btn-grid');
1018-      const keys = Object.keys(btns);
1019-      if (grid.children.length !== keys.length) {
1020-        grid.innerHTML = '';
1021-        keys.forEach(k => {
1022-          const d = document.createElement('div');
1023-          d.className = 'btn-dot';
1024-          d.id = 'btn-' + k;
1025-          d.textContent = k.replace('btn_', '');
1026-          grid.appendChild(d);
1027-        });
1028-      }
1029-      keys.forEach(k => {
1030-        const el = document.getElementById('btn-' + k);
1031-        if (el) el.className = 'btn-dot' + (btns[k] ? ' pressed' : '');
1032-      });
1033-    }
1034-
1035-    // ─── Steering Wheel Canvas ──────────────────────────────────────────────────
1036-
1037-    const canvas = document.getElementById('wheel-canvas');
1038-    const ctx = canvas.getContext('2d');
1039-    const W = 320, H = 320, CX = 160, CY = 160;
1040-
1041-    function drawWheel(angle) {
1042-      ctx.clearRect(0, 0, W, H);
1043-      const rad = (angle * Math.PI) / 180;
1044-
1045-      ctx.save();
1046-      ctx.translate(CX, CY);
1047-      ctx.rotate(rad);
1048-
1049-      // Outer ring
1050-      ctx.beginPath();
1051-      ctx.arc(0, 0, 120, 0, Math.PI * 2);
1052-      ctx.lineWidth = 16;
1053-      ctx.strokeStyle = angle > 1 ? '#ff6d00' : angle < -1 ? '#00e5ff' : '#2a3040';
1054-      ctx.stroke();
1055-
1056-      // Grip tapes
1057-      const grips = [0, Math.PI * 0.5, Math.PI, Math.PI * 1.5];
1058-      grips.forEach(a => {
1059-        ctx.beginPath();
1060-        ctx.arc(0, 0, 120, a - 0.35, a + 0.35);
1061-        ctx.lineWidth = 20;
1062-        ctx.strokeStyle = '#1a2030';
1063-        ctx.stroke();
1064-        ctx.beginPath();
1065-        ctx.arc(0, 0, 120, a - 0.3, a + 0.3);
1066-        ctx.lineWidth = 14;
1067-        ctx.strokeStyle = '#2c3550';
1068-        ctx.stroke();
1069-      });
1070-
1071-      // Spokes
1072-      const spokeAngles = [-Math.PI * 0.25, Math.PI * 0.25, Math.PI * 0.5];
1073-      spokeAngles.forEach(a => {
1074-        ctx.beginPath();
1075-        ctx.moveTo(Math.cos(a) * 28, Math.sin(a) * 28);
1076-        ctx.lineTo(Math.cos(a) * 108, Math.sin(a) * 108);
1077-        ctx.lineWidth = 8;
1078-        ctx.strokeStyle = '#1e2535';
1079-        ctx.stroke();
1080-        ctx.lineWidth = 5;
1081-        ctx.strokeStyle = '#2a3548';
1082-        ctx.stroke();
1083-
1084-        ctx.beginPath();
1085-        ctx.moveTo(Math.cos(a + Math.PI) * 28, Math.sin(a + Math.PI) * 28);
1086-        ctx.lineTo(Math.cos(a + Math.PI) * 108, Math.sin(a + Math.PI) * 108);
1087-        ctx.lineWidth = 8;
1088-        ctx.strokeStyle = '#1e2535';
1089-        ctx.stroke();
1090-        ctx.lineWidth = 5;
1091-        ctx.strokeStyle = '#2a3548';
1092-        ctx.stroke();
1093-      });
1094-
1095-      // Center hub
1096-      ctx.beginPath();
1097-      ctx.arc(0, 0, 28, 0, Math.PI * 2);
1098-      ctx.fillStyle = '#151a24';
1099-      ctx.fill();
1100-      ctx.strokeStyle = '#2a3548';
1101-      ctx.lineWidth = 2;
1102-      ctx.stroke();
1103-
1104-      // Center mark (top indicator)
1105-      ctx.beginPath();
1106-      ctx.moveTo(0, -8);
1107-      ctx.lineTo(0, -20);
1108-      ctx.lineWidth = 3;
1109-      ctx.strokeStyle = angle > 1 ? '#ff6d00' : angle < -1 ? '#00e5ff' : '#5a6070';
1110-      ctx.lineCap = 'round';
1111-      ctx.stroke();
1112-
1113-      ctx.restore();
1114-
1115-      // Fixed reference marks at top
1116-      ctx.save();
1117-      ctx.translate(CX, CY);
1118-      // Center top mark
1119-      ctx.beginPath();
1120-      ctx.moveTo(0, -130);
1121-      ctx.lineTo(0, -142);
1122-      ctx.strokeStyle = '#00e5ff';
1123-      ctx.lineWidth = 2;
1124-      ctx.stroke();
1125-
1126-      // Scale marks -90 and +90
1127-      [-90, 90].forEach(deg => {
1128-        const r = deg * Math.PI / 180;
1129-        const x1 = Math.sin(r) * 130, y1 = -Math.cos(r) * 130;
1130-        const x2 = Math.sin(r) * 142, y2 = -Math.cos(r) * 142;
1131-        ctx.beginPath();
1132-        ctx.moveTo(x1, y1);
1133-        ctx.lineTo(x2, y2);
1134-        ctx.strokeStyle = '#333a48';
1135-        ctx.lineWidth = 1.5;
1136-        ctx.stroke();
1137-      });
1138-
1139-      // Arc indicator for steering angle
1140-      if (Math.abs(angle) > 0.5) {
1141-        ctx.beginPath();
1142-        const startAngle = -Math.PI / 2;
1143-        const endAngle = startAngle + rad;
1144-        ctx.arc(0, 0, 140, Math.min(startAngle, endAngle), Math.max(startAngle, endAngle));
1145-        ctx.strokeStyle = angle > 0 ? 'rgba(255,109,0,0.5)' : 'rgba(0,229,255,0.5)';
1146-        ctx.lineWidth = 4;
1147-        ctx.stroke();
1148-      }
1149-
1150-      ctx.restore();
1151-    }
1152-
1153-    drawWheel(0);
1154-
1155-    // ─── Log ────────────────────────────────────────────────────────────────────
1156-
1157-    function addLog(tag, msg) {
1158-      const box = document.getElementById('esp-log');
1159-      const ts = new Date().toLocaleTimeString('id', { hour12: false });
1160-      const line = document.createElement('div');
1161-      line.className = 'log-line';
1162-      line.innerHTML = `<span class="log-ts">${ts}</span>[${tag}] ${msg}`;
1163-      box.prepend(line);
1164-      if (box.children.length > 50) box.lastChild.remove();
1165-    }
1166-
1167-    // ─── Dots ────────────────────────────────────────────────────────────────────
1168-
1169-    function setDot(id, on, label) {
1170-      const dot = document.getElementById('dot-' + id);
1171-      const lbl = document.getElementById('lbl-' + id);
1172-      if (dot) dot.className = 'dot' + (on ? ' ' + id + '-on' : '');
1173-      if (lbl) lbl.textContent = label;
1174-    }
1175-
1176-    // ─── Calibration ────────────────────────────────────────────────────────────
1177-
1178-    function applyConfig(cfg) {
1179-      const cal = cfg.calibration || {};
1180-      document.getElementById('cal-sc').value = (cal.steering_center ?? 0).toFixed(4);
1181-      document.getElementById('cal-sr').value = (cal.steering_range ?? 1).toFixed(4);
1182-      document.getElementById('cal-ma').value = cal.steering_max_angle ?? 90;
1183-      document.getElementById('cal-ds').value = (cal.deadzone_steering ?? 0.02).toFixed(3);
1184-      document.getElementById('cal-tm').value = (cal.throttle_min ?? -1).toFixed(4);
1185-      document.getElementById('cal-dp').value = (cal.deadzone_pedal ?? 0.02).toFixed(3);
1186-    }
1187-
1188-    function saveCalibration() {
1189-      if (!ws || ws.readyState !== WebSocket.OPEN) {
1190-        addLog('ERR', 'Tidak terhubung ke server!');
1191-        return;
1192-      }
1193-      const fields = [
1194-        { section: 'calibration', key: 'steering_center', id: 'cal-sc' },
1195-        { section: 'calibration', key: 'steering_range', id: 'cal-sr' },
1196-        { section: 'calibration', key: 'steering_max_angle', id: 'cal-ma' },
1197-        { section: 'calibration', key: 'deadzone_steering', id: 'cal-ds' },
1198-        { section: 'calibration', key: 'throttle_min', id: 'cal-tm' },
1199-        { section: 'calibration', key: 'deadzone_pedal', id: 'cal-dp' },
1200-      ];
1201-      fields.forEach(f => {
1202-        ws.send(JSON.stringify({
1203-          type: 'update_config',
1204-          section: f.section,
1205-          key: f.key,
1206-          value: parseFloat(document.getElementById(f.id).value)
1207-        }));
1208-      });
1209-      addLog('CAL', 'Mengirim data kalibrasi...');
1210-    }
1211-
1212-    // ─── RTSP Streaming ────────────────────────────────────────────────────────
1213-    
1214-    function startRTSP() {
1215-      const url = document.getElementById('rtsp-url').value.trim();
1216-      if (!url) {
1217-        addLog('ERR', 'Masukkan URL RTSP!');
1218-        return;
1219-      }
1220-
1221-      addLog('SYS', 'Menghubungkan ke RTSP via Bridge...');
1222-      
1223-      // Update bridge server about the new RTSP URL
1224-      fetch('http://localhost:5000/set_rtsp', {
1225-        method: 'POST',
1226-        headers: { 'Content-Type': 'application/json' },
1227-        body: JSON.stringify({ url: url })
1228-      })
1229-      .then(res => res.json())
1230-      .then(data => {
1231-        addLog('SYS', 'RTSP Bridge OK. Memulai stream...');
1232-        const videoBg = document.getElementById('video-bg');
1233-        const container = document.getElementById('video-bg-container');
1234-        
1235-        // Cache busting for the image stream
1236-        videoBg.src = 'http://localhost:5000/video_feed?t=' + Date.now();
1237-        container.style.display = 'block';
1238-        document.body.classList.add('floating-mode');
1239-        
1240-        document.getElementById('btn-rtsp').textContent = 'STOP';
1241-        document.getElementById('btn-rtsp').onclick = stopRTSP;
1242-      })
1243-      .catch(err => {
1244-        addLog('ERR', 'Gagal hubung ke Bridge (pastikan video_bridge.py jalan)');
1245-      });
1246-    }
1247-
1248-    function stopRTSP() {
1249-      const videoBg = document.getElementById('video-bg');
1250-      const container = document.getElementById('video-bg-container');
1251-      
1252-      videoBg.src = '';
1253-      container.style.display = 'none';
1254-      document.body.classList.remove('floating-mode');
1255-      
1256-      document.getElementById('btn-rtsp').textContent = 'STREAM';
1257-      document.getElementById('btn-rtsp').onclick = startRTSP;
1258-      addLog('SYS', 'Stream dihentikan');
1259-    }
1260-
1261-    function togglePanel(side) {
1262-      const panel = document.getElementById('panel-' + side);
1263-      const grid = document.querySelector('.grid');
1264-      
1265-      panel.classList.toggle('hidden');
1266-      
1267-      const leftHidden = document.getElementById('panel-left').classList.contains('hidden');
1268-      const rightHidden = document.getElementById('panel-right').classList.contains('hidden');
1269-      
1270-      grid.classList.remove('no-side-panels', 'no-left-panel', 'no-right-panel');
1271-      
1272-      if (leftHidden && rightHidden) {
1273-        grid.classList.add('no-side-panels');
1274-      } else if (leftHidden) {
1275-        grid.classList.add('no-left-panel');
1276-      } else if (rightHidden) {
1277-        grid.classList.add('no-right-panel');
1278-      }
1279-    }
1280-
1281-    // ─── Auto connect on load ─────────────────────────────────────────────────────
1282-
1283-    window.addEventListener('load', () => {
1284-      const url = document.getElementById('ws-url').value;
1285-      connectWS(url);
1286-      addLog('SYS', 'Dashboard siap. Menghubungkan ke server...');
1287-    });
1288-  </script>
