from flask import (
    Flask,
    request,
    render_template_string,
    redirect,
    url_for,
    send_from_directory,
    jsonify,
)
import os
import datetime
import subprocess
import threading
import requests

app = Flask(__name__)
UPLOAD_FOLDER = "uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# App State
state = {
    "current_label": "noise",
    "samples": [],
    "counts": {},
    "esp_ip": None,  # Registered ESP32 IP
    "is_collecting": False,
}
state_lock = threading.Lock()

HTML_TEMPLATE = """
<!DOCTYPE html>
<html>
<head>
    <title>Edge Impulse Data Collector</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: sans-serif; max-width: 600px; margin: auto; padding: 20px; background: #f4f4f9; }
        .card { background: white; padding: 20px; border-radius: 8px; box-shadow: 0 2px 5px rgba(0,0,0,0.1); margin-bottom: 20px; }
        .btn { padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; color: white; font-weight: bold; margin-right: 10px; }
        .btn-blue { background: #007bff; }
        .btn-red { background: #dc3545; }
        .btn-green { background: #28a745; }
        .btn-orange { background: #fd7e14; }
        .btn-disabled { background: #ccc; cursor: not-allowed; }
        input[type="text"] { padding: 10px; width: 60%; border: 1px solid #ddd; border-radius: 4px; }
        .sample-item { display: flex; justify-content: space-between; align-items: center; padding: 10px; border-bottom: 1px solid #eee; }
        .status-on { color: green; font-weight: bold; }
        .status-off { color: red; font-weight: bold; }
    </style>
</head>
<body>
    <h1>🎤 Audio Collection Center</h1>
    
    <div class="card">
        <h3>Device Status</h3>
        <p>ESP32 IP: <b>{{ esp_ip if esp_ip else "Not Registered" }}</b></p>
        <p>Collecting: <span class="{{ 'status-on' if is_collecting else 'status-off' }}">{{ "YES" if is_collecting else "NO" }}</span></p>
        
        <hr>
        <button class="btn {{ 'btn-green' if not is_collecting else 'btn-disabled' }}" 
                onclick="location.href='/esp_control?cmd=start'" {{ 'disabled' if is_collecting or not esp_ip }}>START CAPTURE</button>
        
        <button class="btn {{ 'btn-red' if is_collecting else 'btn-disabled' }}" 
                onclick="location.href='/esp_control?cmd=stop'" {{ 'disabled' if not is_collecting or not esp_ip }}>STOP</button>
    </div>

    <div class="card">
        <h3>Label Settings</h3>
        <p>Current: <b style="color:blue">{{ current_label }}</b></p>
        <form action="/set_label" method="GET">
            <input type="text" name="label" placeholder="New label..." value="{{ current_label }}">
            <button class="btn btn-blue" type="submit">Set</button>
        </form>
        <div style="margin-top:10px">
            <button class="btn btn-orange" style="padding:5px" onclick="location.href='/set_label?label=noise'">Noise</button>
            <button class="btn btn-orange" style="padding:5px" onclick="location.href='/set_label?label=hey_esp'">Keyword</button>
        </div>
    </div>

    <div class="card">
        <h3>Statistics</h3>
        <table style="width: 100%; border-collapse: collapse;">
            <tr style="border-bottom: 2px solid #eee;">
                <th style="text-align: left; padding: 8px;">Label</th>
                <th style="text-align: right; padding: 8px;">Samples</th>
            </tr>
            {% for label, count in counts.items() %}
            <tr style="border-bottom: 1px solid #eee;">
                <td style="padding: 8px;"><b>{{ label }}</b></td>
                <td style="text-align: right; padding: 8px;">{{ count }}</td>
            </tr>
            {% endfor %}
            {% if not counts %}
            <tr><td colspan="2" style="text-align: center; padding: 10px; color: #999;">No data yet</td></tr>
            {% endif %}
        </table>
        <form action="/reset_counts" method="POST" style="margin-top: 15px;">
            <button class="btn btn-red" style="width: 100%; padding: 5px;" onclick="return confirm('Reset all counters?')">Reset Statistics</button>
        </form>
    </div>

    <div class="card">
        <h3>Last 10 Samples</h3>
        {% for s in samples %}
        <div class="sample-item">
            <span><b>{{ s.label }}</b><br><small>{{ s.time }}</small></span>
            <audio controls src="/file/{{ s.name }}" style="height: 30px; width: 150px;"></audio>
            <form action="/delete/{{ s.name }}" method="POST"><button class="btn btn-red" style="padding: 5px 10px;">X</button></form>
        </div>
        {% endfor %}
    </div>

    <button class="btn btn-blue" onclick="location.reload()" style="width: 100%;">Refresh Dashboard</button>
</body>
</html>
"""


@app.route("/")
def index():
    return render_template_string(
        HTML_TEMPLATE,
        current_label=state["current_label"],
        samples=state["samples"][:10],
        esp_ip=state["esp_ip"],
        is_collecting=state["is_collecting"],
        counts=state["counts"],
    )


@app.route("/reset_counts", methods=["POST"])
def reset_counts():
    with state_lock:
        state["counts"] = {}
    return redirect(url_for("index"))


@app.route("/register", methods=["POST"])
def register():
    data = request.json
    print(f"Received registration: {data}")
    if not data or "ip" not in data:
        return jsonify({"status": "error", "message": "No IP provided"}), 400

    with state_lock:
        state["esp_ip"] = data.get("ip")
    print(f"Device registered successfully: {state['esp_ip']}")
    return jsonify({"status": "registered"}), 200


@app.route("/esp_control")
def esp_control():
    cmd = request.args.get("cmd")
    print(f"Control command received: {cmd}")
    if not state["esp_ip"]:
        print("Error: No device registered")
        return "No device registered", 400

    try:
        url = f"http://{state['esp_ip']}/control?cmd={cmd}"
        print(f"Sending command to ESP32: {url}")
        resp = requests.get(url, timeout=5)
        print(f"ESP32 response: {resp.status_code} - {resp.text}")
        if resp.status_code == 200:
            with state_lock:
                state["is_collecting"] = cmd == "start"
            return redirect(url_for("index"))
    except Exception as e:
        print(f"Error contacting ESP32: {e}")
        return f"Error contacting ESP32: {e}", 500

    return "Failed", 400


@app.route("/upload", methods=["POST"])
def upload_file():
    print(f"Receiving audio upload...")
    with state_lock:
        label = state["current_label"]
        count = state["counts"].get(label, 0) + 1
        state["counts"][label] = count
        filename = f"{label}.{count}.wav"
        filepath = os.path.join(UPLOAD_FOLDER, filename)

        data = request.get_data()
        print(f"Saving {len(data)} bytes to {filename}")
        with open(filepath, "wb") as f:
            f.write(data)

        state["samples"].insert(
            0,
            {
                "name": filename,
                "label": label,
                "time": datetime.datetime.now().strftime("%H:%M:%S"),
            },
        )
    print(f"Upload complete: {filename}")
    return "OK", 200


@app.route("/file/<path:filename>")
def get_file(filename):
    return send_from_directory(UPLOAD_FOLDER, filename)


@app.route("/delete/<filename>", methods=["POST"])
def delete_file(filename):
    filepath = os.path.join(UPLOAD_FOLDER, filename)
    if os.path.exists(filepath):
        os.remove(filepath)
    with state_lock:
        state["samples"] = [s for s in state["samples"] if s["name"] != filename]
    return redirect(url_for("index"))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
