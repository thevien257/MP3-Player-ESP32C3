#!/usr/bin/env python3
import time
import subprocess
import threading
import os
import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib
from flask import Flask, render_template_string, request, jsonify

# Initialize GStreamer
Gst.init(None)

app = Flask(__name__)

# --- Global State ---
playlist = []
current_pipeline = None       # GStreamer Pipeline object
current_fetch_process = None # yt-dlp process
current_song = None
is_paused = False
is_fetching = False          
is_buffering = False         
current_index = 0
manual_switch = False        
esp32_ip = "10.62.65.36"
FIXED_VOLUME = 0.3           
gst_mainloop = None
gst_mainloop_thread = None

# [DOWNLOAD STATE]
download_status = "Idle"
is_downloading = False
download_filename = ""

if not os.path.exists('downloads'):
    os.makedirs('downloads')

# Start GLib MainLoop for GStreamer bus messages
def run_gst_mainloop():
    global gst_mainloop
    gst_mainloop = GLib.MainLoop()
    gst_mainloop.run()

gst_mainloop_thread = threading.Thread(target=run_gst_mainloop, daemon=True)
gst_mainloop_thread.start()

# --- HTML UI ---
HTML = """
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Streamer & MP3 Downloader</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;600&display=swap" rel="stylesheet">
    <style>
        :root {
            --primary: #2563eb; --secondary: #8b5cf6; --bg: #f3f4f6; --card-bg: #ffffff;
            --text-main: #1f2937; --text-sub: #6b7280; --border: #e5e7eb;
            --active-bg: #eff6ff; --active-text: #1d4ed8; --warn: #f59e0b; --danger: #ef4444;
        }
        body { font-family: 'Inter', sans-serif; background-color: var(--bg); color: var(--text-main); margin: 0; padding: 20px; display: flex; justify-content: center; min-height: 100vh; }
        .container { width: 100%; max-width: 600px; background: var(--card-bg); border-radius: 16px; box-shadow: 0 4px 20px rgba(0, 0, 0, 0.08); padding: 24px; display: flex; flex-direction: column; height: 90vh; }
        h1 { margin: 0 0 20px 0; font-size: 1.5rem; text-align: center; }
        .input-group { display: flex; gap: 10px; margin-bottom: 10px; }
        input[type="text"] { width: 100%; padding: 12px; border: 1px solid var(--border); border-radius: 8px; outline: none; transition: 0.2s; box-sizing: border-box; }
        input[type="text"]:focus { border-color: var(--primary); }
        button { padding: 10px 16px; border: none; border-radius: 8px; font-weight: 600; cursor: pointer; transition: 0.2s; flex-shrink: 0; }
        .btn-add { background: var(--primary); color: white; width: 100px;}
        .btn-add:disabled { background: var(--text-sub); }
        .btn-add:hover { background: #1d4ed8; }
        .status-bar { margin-bottom: 10px; font-size: 0.85rem; color: var(--text-sub); min-height: 20px; display: flex; flex-direction: column; gap: 5px; }
        .dl-status-text { font-size: 0.8rem; color: var(--secondary); font-weight: 600; text-align: right;}
        .loading-bar { height: 4px; width: 100%; background: #e5e7eb; margin-top: 5px; display: none; overflow: hidden; border-radius: 2px; }
        .loading-bar div { height: 100%; width: 50%; background: var(--primary); animation: loading 1s infinite linear; }
        @keyframes loading { 0% { transform: translateX(-100%); } 100% { transform: translateX(200%); } }
        .now-playing { background: linear-gradient(135deg, #f0f9ff 0%, #e0f2fe 100%); border: 1px solid #bae6fd; border-radius: 12px; padding: 20px; text-align: center; margin: 10px 0 20px 0; position: relative; }
        .np-label { font-size: 0.75rem; color: var(--primary); font-weight: 600; text-transform: uppercase;}
        .np-title { font-size: 1.0rem; font-weight: 600; margin: 8px 0; word-break: break-word; }
        .status-badge { display: inline-flex; align-items: center; gap: 6px; font-size: 0.75rem; padding: 4px 12px; border-radius: 20px; background: #fff; color: var(--text-sub); border: 1px solid var(--border); }
        .spinner { width: 10px; height: 10px; border: 2px solid var(--warn); border-bottom-color: transparent; border-radius: 50%; display: inline-block; animation: rotation 1s linear infinite; }
        @keyframes rotation { 0% { transform: rotate(0deg); } 100% { transform: rotate(360deg); } }
        
        .controls-area { border-bottom: 1px solid var(--border); padding-bottom: 20px; margin-bottom: 10px;}
        .controls { display: grid; grid-template-columns: 1fr 1fr 1fr 1fr; gap: 10px; }
        .btn-control { background: white; border: 1px solid var(--border); }
        .btn-stop { color: #ef4444; background: #fef2f2; border-color: #fecaca; }

        .queue-header { padding: 15px 0 10px 0; font-weight: 600; color: var(--text-sub); display: flex; justify-content: space-between;}
        .playlist-wrapper { flex-grow: 1; overflow-y: auto; border: 1px solid var(--border); border-radius: 8px; background: #f9fafb; }
        .song-list { list-style: none; padding: 0; margin: 0; }
        .song-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 15px; border-bottom: 1px solid var(--border); background: white; cursor: pointer; transition: 0.1s; }
        .song-item:hover { background: #f3f4f6; }
        .song-item.active { background-color: var(--active-bg); border-left: 4px solid var(--primary); }
        .song-item.active .song-title { color: var(--active-text); font-weight: 700; }
        .song-info { display: flex; align-items: center; gap: 10px; overflow: hidden; flex-grow: 1; margin-right: 10px; }
        .song-title { font-size: 0.9rem; font-weight: 500; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; color: var(--text-main); }
        .list-actions { display: flex; gap: 8px; align-items: center; }
        .btn-icon-dl { width: 90px; padding: 8px 0; font-size: 0.75rem; font-weight: 600; background: white; color: var(--secondary); border: 1px solid var(--secondary); border-radius: 6px; flex-shrink: 0; display: flex; justify-content: center; align-items: center; transition: 0.2s; }
        .btn-icon-dl:hover { background: var(--secondary); color: white; }
        .btn-icon-dl:disabled { opacity: 0.5; cursor: not-allowed; border-color: #ccc; color: #ccc;}
        .play-indicator { color: var(--primary); font-size: 1rem; opacity: 0; width: 15px; flex-shrink: 0;}
        .song-item.active .play-indicator { opacity: 1; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎵 ESP32 Hub</h1>
        <input type="text" id="ip" value="{{ default_ip }}" placeholder="ESP32 IP" style="margin-bottom: 8px;">
        <div class="input-group">
            <input type="text" id="url" placeholder="Paste YouTube Link / Playlist">
            <button id="btn-add" class="btn-add" onclick="addToQueue()">+ Add</button>
        </div>
        <div class="status-bar">
            <div style="display:flex; justify-content:space-between;">
                <span id="system-status">Ready</span>
                <span id="dl-status" class="dl-status-text"></span>
            </div>
            <div id="loading" class="loading-bar"><div></div></div>
        </div>
        <div class="now-playing">
            <div class="np-label">Now Playing</div>
            <div class="np-title" id="np-title">Waiting for music...</div>
            <div id="np-status" class="status-badge">Idle</div>
            <div id="buffering-bar" class="loading-bar" style="margin-top:15px; height:2px; display:none;"><div></div></div>
        </div>
        
        <div class="controls-area">
            <div class="controls">
                <button class="btn-control btn-stop" onclick="control('stop')">Stop & Clear</button>
                <button class="btn-control" onclick="control('pause')">Pause</button>
                <button class="btn-control" onclick="control('resume')">Play</button>
                <button class="btn-control" onclick="control('next')">Next</button>
            </div>
        </div>

        <div class="queue-header">
            <span>Playlist</span>
            <span id="queue-count" style="font-size: 0.85rem;">0 songs</span>
        </div>
        <div class="playlist-wrapper">
            <ul id="playlist-container" class="song-list"></ul>
        </div>
    </div>
    <script>
        function updateUI() {
            fetch('/status').then(r => r.json()).then(data => {
                const loadingBar = document.getElementById('loading');
                const sysStatus = document.getElementById('system-status');
                const btnAdd = document.getElementById('btn-add');
                const dlStatus = document.getElementById('dl-status');

                if (data.is_fetching) {
                    loadingBar.style.display = 'block'; sysStatus.innerText = "Fetching playlist details..."; btnAdd.disabled = true;
                } else {
                    loadingBar.style.display = 'none'; btnAdd.disabled = false; sysStatus.innerText = data.is_playing ? "Streaming active" : "Ready";
                }

                if (data.is_downloading) {
                    dlStatus.innerText = `Downloading: ${data.download_filename}...`; dlStatus.style.color = "var(--secondary)";
                } else {
                    dlStatus.innerText = data.download_msg;
                    if(data.download_msg === "Failed!") dlStatus.style.color = "var(--danger)";
                    else if(data.download_msg === "Completed!") dlStatus.style.color = "#059669";
                    else dlStatus.style.color = "var(--text-sub)";
                }

                document.getElementById('np-title').innerText = data.current_display_title || "Waiting for music...";
                const badge = document.getElementById('np-status');
                const buffBar = document.getElementById('buffering-bar');

                if (data.is_buffering) {
                    badge.innerHTML = '<span class="spinner"></span> LOADING...'; badge.style.color = "var(--warn)"; badge.style.borderColor = "var(--warn)"; buffBar.style.display = 'block';
                } else if (data.is_playing && !data.is_paused) {
                    badge.innerHTML = 'LIVE 🎵'; badge.style.color = "#059669"; badge.style.borderColor = "#059669"; buffBar.style.display = 'none';
                } else if (data.is_paused) {
                    badge.innerHTML = 'PAUSED'; badge.style.color = "#d97706"; badge.style.borderColor = "#d97706"; buffBar.style.display = 'none';
                } else {
                    badge.innerHTML = 'IDLE'; badge.style.color = "#6b7280"; badge.style.borderColor = "#e5e7eb"; buffBar.style.display = 'none';
                }

                document.getElementById('queue-count').innerText = data.queue.length + " songs";
                const list = document.getElementById('playlist-container');
                list.innerHTML = '';
                
                data.queue.forEach((song, index) => {
                    const li = document.createElement('li');
                    li.className = 'song-item';
                    if (index === data.current_index) li.classList.add('active');
                    li.onclick = (e) => { playSpecific(index); };
                    const isDownloadingThis = (data.is_downloading);
                    li.innerHTML = `
                        <div class="song-info">
                            <span class="play-indicator">▶</span>
                            <span class="song-title">${index + 1}. ${song.title}</span>
                        </div>
                        <div class="list-actions">
                            <button class="btn-icon-dl" onclick="event.stopPropagation(); downloadSong(${index})" ${isDownloadingThis ? 'disabled' : ''}>⬇ MP3</button>
                        </div>
                    `;
                    list.appendChild(li);
                });
            });
        }

        function addToQueue() {
            const url = document.getElementById('url').value; const ip = document.getElementById('ip').value;
            if(!url) return;
            fetch('/add', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({url: url, ip: ip}) }).then(() => { document.getElementById('url').value = ''; updateUI(); });
        }
        function downloadSong(index) {
            fetch('/download_index', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({index: index}) }).then(() => updateUI());
        }
        function control(action) { fetch('/control/' + action, {method: 'POST'}).then(updateUI); }
        function playSpecific(index) {
            fetch('/play_index', { method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify({index: index}) }).then(updateUI);
        }
        setInterval(updateUI, 1000); updateUI();
    </script>
</body>
</html>
"""

# --- Routes ---

@app.route('/')
def index():
    return render_template_string(HTML, default_ip=esp32_ip)

@app.route('/status')
def status():
    display_title = None
    if current_song:
        display_title = current_song['title']

    return jsonify({
        "current_display_title": display_title,
        "is_playing": current_pipeline is not None,
        "is_paused": is_paused,
        "is_fetching": is_fetching,
        "is_buffering": is_buffering,
        "queue": playlist,
        "current_index": current_index,
        "is_downloading": is_downloading,
        "download_msg": download_status,
        "download_filename": download_filename
    })

@app.route('/add', methods=['POST'])
def add_route():
    global esp32_ip
    data = request.json
    url = data.get('url')
    esp32_ip = data.get('ip', esp32_ip)
    threading.Thread(target=fetch_metadata_and_add, args=(url,), daemon=True).start()
    return jsonify({"status": "started_fetching"})

@app.route('/control/<action>', methods=['POST'])
def control_route(action):
    global current_pipeline, is_paused, playlist, current_song, current_index, manual_switch
    
    if action == 'stop':
        playlist = []
        current_song = None
        current_index = 0
        kill_current_pipeline()
        
    elif action == 'next':
        if len(playlist) > 0:
            manual_switch = True 
            current_index = (current_index + 1) % len(playlist)
            kill_current_pipeline() 
    
    elif action == 'pause':
        if current_pipeline and not is_paused:
            # Properly pause the GStreamer pipeline
            ret = current_pipeline.set_state(Gst.State.PAUSED)
            if ret == Gst.StateChangeReturn.FAILURE:
                print("[!] Failed to pause pipeline")
            else:
                is_paused = True
                print("[*] Pipeline paused")
    
    elif action == 'resume':
        if current_pipeline and is_paused:
            # Properly resume the GStreamer pipeline
            ret = current_pipeline.set_state(Gst.State.PLAYING)
            if ret == Gst.StateChangeReturn.FAILURE:
                print("[!] Failed to resume pipeline")
            else:
                is_paused = False
                print("[*] Pipeline resumed")
    
    return jsonify({"status": "ok"})

@app.route('/play_index', methods=['POST'])
def play_index():
    global playlist, current_index, manual_switch
    idx = request.json.get('index')
    if 0 <= idx < len(playlist):
        manual_switch = True 
        current_index = idx 
        kill_current_pipeline()
    return jsonify({"status": "ok"})

@app.route('/download_index', methods=['POST'])
def download_index_route():
    idx = request.json.get('index')
    if 0 <= idx < len(playlist):
        target_song = playlist[idx]
        threading.Thread(target=run_download, args=(target_song,), daemon=True).start()
        return jsonify({"status": "started_download"})
    return jsonify({"status": "error", "msg": "Invalid index"})

# --- Helper Functions ---

def kill_current_pipeline():
    global current_pipeline, current_fetch_process, is_paused, is_buffering
    
    is_buffering = False
    is_paused = False
    
    if current_pipeline:
        try:
            # Stop the pipeline gracefully
            current_pipeline.set_state(Gst.State.NULL)
            current_pipeline = None
            print("[*] Pipeline stopped")
        except Exception as e:
            print(f"[!] Error stopping pipeline: {e}")
            current_pipeline = None

    if current_fetch_process:
        print("[!] Killing yt-dlp fetch process...")
        try:
            current_fetch_process.terminate()
            current_fetch_process.wait(timeout=1)
        except:
            try:
                current_fetch_process.kill()
            except:
                pass
        current_fetch_process = None

def fetch_metadata_and_add(url):
    global is_fetching
    is_fetching = True
    print(f"[*] Fetching info for: {url}")
    cmd = ["yt-dlp", "--flat-playlist", "--print", "%(title)s::%(id)s", url]
    try:
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        lines = res.stdout.strip().split('\n')
        count = 0
        for line in lines:
            if "::" in line:
                title, vid_id = line.split("::", 1)
                playlist.append({
                    "title": title,
                    "id": vid_id,
                    "url": f"https://www.youtube.com/watch?v={vid_id}"
                })
                count += 1
        print(f"[*] Added {count} songs to playlist.")
    except Exception as e:
        print(f"[!] Error fetching playlist: {e}")
    finally:
        is_fetching = False

def run_download(song_obj):
    global is_downloading, download_status, download_filename
    
    if is_downloading: 
        return
    
    is_downloading = True
    download_filename = song_obj['title']
    download_status = "Starting..."
    
    cmd = [
        "yt-dlp", "-o", "downloads/%(title)s.%(ext)s", "-f", "bestaudio/best",
        "-x", "--audio-format", "mp3", "--audio-quality", "0",
        "--no-playlist", song_obj['url']
    ]
    
    print(f"[*] Downloading: {song_obj['title']}")
    try:
        subprocess.run(cmd, check=True, timeout=600)
        download_status = "Completed!"
    except subprocess.CalledProcessError:
        download_status = "Failed!"
    except subprocess.TimeoutExpired:
        download_status = "Timeout!"
    except Exception as e:
        download_status = f"Error: {str(e)}"
    finally:
        is_downloading = False
        time.sleep(3)
        if not is_downloading:
            download_status = "Idle"
            download_filename = ""

def player_loop():
    global current_pipeline, current_fetch_process, current_song, is_paused, playlist, current_index, manual_switch, is_buffering
    
    while True:
        if current_pipeline is None and len(playlist) > 0:
            if current_index >= len(playlist): 
                current_index = 0
            
            current_song = playlist[current_index] 
            print(f"[*] Buffering index {current_index}: {current_song['title']}")
            is_buffering = True 
            
            try:
                cmd_url = ["yt-dlp", "-f", "bestaudio", "-g", current_song['url']]
                current_fetch_process = subprocess.Popen(
                    cmd_url, 
                    stdout=subprocess.PIPE, 
                    stderr=subprocess.PIPE, 
                    text=True
                )
                
                stdout, stderr = current_fetch_process.communicate(timeout=30)
                
                if current_fetch_process is None: 
                    print("[!] Fetch aborted via Manual Switch.")
                    is_buffering = False
                    continue 

                return_code = current_fetch_process.returncode
                current_fetch_process = None

                if return_code != 0:
                    print(f"[!] Failed to get stream URL for: {current_song['title']}")
                    print(f"[!] Error: {stderr[:200]}")
                    
                    if manual_switch:
                        print("[!] Manual song failed. Retrying...")
                        is_buffering = False
                        time.sleep(1)
                        continue
                    else:
                        if len(playlist) > 0:
                            current_index = (current_index + 1) % len(playlist)
                        is_buffering = False
                        continue 
                
                audio_url = stdout.strip().split('\n')[0]
                if not audio_url:
                    print("[!] Empty audio URL received")
                    is_buffering = False
                    continue

                # --- BUILD GSTREAMER PIPELINE ---
                print(f"[*] Creating GStreamer pipeline with Volume: {FIXED_VOLUME}")
                
                pipeline_str = (
                    f"urisourcebin uri={audio_url} ! "
                    f"decodebin ! audioconvert ! volume volume={FIXED_VOLUME} ! "
                    f"audioresample ! opusenc inband-fec=true frame-size=20 bandwidth=mediumband ! "
                    f"rtpopuspay pt=96 ! udpsink host={esp32_ip} port=5004"
                )
                
                current_pipeline = Gst.parse_launch(pipeline_str)
                
                if not current_pipeline:
                    print("[!] Failed to create pipeline")
                    is_buffering = False
                    continue
                
                # Set up bus message handler
                bus = current_pipeline.get_bus()
                bus.add_signal_watch()
                bus.connect("message", on_bus_message)
                
                # Start playing
                ret = current_pipeline.set_state(Gst.State.PLAYING)
                if ret == Gst.StateChangeReturn.FAILURE:
                    print("[!] Unable to set pipeline to PLAYING")
                    current_pipeline = None
                    is_buffering = False
                    continue
                
                is_buffering = False 
                
                if manual_switch:
                    manual_switch = False
                
                # Wait for EOS or error
                while current_pipeline:
                    time.sleep(0.5)
                
                # Clean up after playback ends
                if len(playlist) == 0:
                    current_index = 0
                    current_song = None
                elif manual_switch:
                    pass 
                else:
                    current_index = (current_index + 1) % len(playlist)
                
            except subprocess.TimeoutExpired:
                print(f"[!] Timeout fetching stream URL")
                if current_fetch_process:
                    current_fetch_process.kill()
                    current_fetch_process = None
                is_buffering = False
                time.sleep(1)
                
            except Exception as e:
                print(f"[!] Playback error: {e}")
                current_pipeline = None
                current_fetch_process = None
                is_buffering = False
                time.sleep(1)
        else:
            time.sleep(0.5)

def on_bus_message(bus, message):
    global current_pipeline, is_paused
    
    t = message.type
    
    if t == Gst.MessageType.EOS:
        print("[*] End of stream")
        if current_pipeline:
            current_pipeline.set_state(Gst.State.NULL)
            current_pipeline = None
            is_paused = False
    
    elif t == Gst.MessageType.ERROR:
        err, debug = message.parse_error()
        print(f"[!] Error: {err}, {debug}")
        if current_pipeline:
            current_pipeline.set_state(Gst.State.NULL)
            current_pipeline = None
            is_paused = False

if __name__ == '__main__':
    threading.Thread(target=player_loop, daemon=True).start()
    print("\n🎵 ESP32 Streamer & Downloader")
    print(f"Open: http://localhost:5000\n")
    app.run(host='0.0.0.0', port=5000, debug=False)