from flask import Flask, request, jsonify, send_from_directory, redirect, url_for
import os
import datetime
import threading
import requests

app = Flask(__name__, static_folder="static", static_url_path="")
UPLOAD_FOLDER = "uploads"
os.makedirs(UPLOAD_FOLDER, exist_ok=True)

# App State
state = {
    "current_label": "noise",
    "samples": [],
    "counts": {},
    "esp_ip": None,
    "is_collecting": False,
}
state_lock = threading.Lock()


@app.before_request
def log_request_info():
    # Filter out noisy polls and pings
    if request.path in ["/api/status", "/"]:
        return
    print(f"\n[Incoming] {request.remote_addr} {request.method} {request.path}")


@app.route("/")
def index():
    return send_from_directory("static", "index.html")


@app.route("/api/status")
def get_status():
    with state_lock:
        return jsonify(
            {
                "current_label": state["current_label"],
                "esp_ip": state["esp_ip"],
                "is_collecting": state["is_collecting"],
                "counts": state["counts"],
                "samples": state["samples"][:10],
            }
        )


@app.route("/register", methods=["POST"])
def register():
    data = request.json
    if not data:
        return jsonify({"status": "error"}), 400

    # If ESP32 reports 0.0.0.0, use the request's source IP
    ip = data.get("ip")
    if not ip or ip == "0.0.0.0":
        ip = request.remote_addr

    with state_lock:
        state["esp_ip"] = ip
    print(f"Device registered: {ip}")
    return jsonify({"status": "registered", "ip": ip}), 200


@app.route("/esp_control")
def esp_control():
    cmd = request.args.get("cmd")
    if not state["esp_ip"]:
        return jsonify({"status": "error", "message": "No device"}), 400

    target_ip = state["esp_ip"].strip().rstrip("/")
    try:
        url = f"http://{target_ip}/control?cmd={cmd}"
        print(f"Forwarding to ESP32: {url}")
        resp = requests.get(url, timeout=3)
        if resp.status_code == 200:
            with state_lock:
                state["is_collecting"] = cmd == "start"
            return jsonify({"status": "ok", "collecting": state["is_collecting"]})
    except Exception as e:
        print(f"Control Error: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500
    return jsonify({"status": "failed"}), 400


@app.route("/set_label")
def set_label():
    label = request.args.get("label", "sample")
    with state_lock:
        state["current_label"] = label
    print(f"Label changed to: {label}")
    return jsonify({"status": "ok", "current_label": label})


@app.route("/upload", methods=["POST"])
def upload_file():
    with state_lock:
        label = state["current_label"]
        count = state["counts"].get(label, 0) + 1
        state["counts"][label] = count
        filename = f"{label}.{count}.wav"
        filepath = os.path.join(UPLOAD_FOLDER, filename)

        data = request.get_data()
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
    print(f"Stored Audio: {filename} ({len(data)} bytes)")
    return jsonify({"status": "ok", "filename": filename}), 200


@app.route("/reset_counts", methods=["POST"])
def reset_counts():
    with state_lock:
        state["counts"] = {}
    print("Statistics reset.")
    return jsonify({"status": "ok"})


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
    print(f"Deleted: {filename}")
    return jsonify({"status": "ok"})


if __name__ == "__main__":
    print("Server starting on http://0.0.0.0:5000")
    app.run(host="0.0.0.0", port=5000, debug=False)
