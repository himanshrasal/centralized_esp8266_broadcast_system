from flask import Flask, request, jsonify
from datetime import datetime
import sqlite3

DB_NAME = "database.db"

def init_db():
    conn = sqlite3.connect(DB_NAME, timeout=5)
    c = conn.cursor()

    c.execute('''
            CREATE TABLE IF NOT EXISTS sensor_data (
              id INTEGER PRIMARY KEY AUTOINCREMENT,
              name TEXT,
              type TEXT,
              isAnalog BOOL,
              value REAL,
              timestamp TEXT
              )
              ''')
    conn.commit()
    conn.close()

init_db()

app = Flask(__name__)

@app.route('/data', methods=['POST'])
def receive_data():
    data = request.get_json()
    
    if not data:
        return jsonify({"status": "error", "message": "No JSON receieved"}), 400

    name = data.get("name")
    sensor_type = data.get("type")
    value = data.get("value")
    isAnalog = data.get("isAnalog")
    time_stamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    if name is None:
        return jsonify({"status": "error", "message": "Missing name"}), 400

    if sensor_type is None or value is None:
        return jsonify({"status": "error", "message": "Missing Fields"}), 400
    
    try:
        value = float(value)
    except (TypeError, ValueError):
        return jsonify({"status": "error", "message": "Invalid value"}), 400
    
    if isAnalog is None:
        return jsonify({"status": "error", "message": "Missing isAnalog"}), 400

    if not isinstance(isAnalog, bool):
        return jsonify({"status": "error", "message": "isAnalog must be boolean"}), 400

    try:
        conn = sqlite3.connect(DB_NAME, timeout=5)
        c = conn.cursor()

        c.execute('''
            INSERT INTO sensor_data (name, type, isAnalog, value, timestamp)
            VALUES (?, ?, ?, ?, ?)
        ''', (name, sensor_type, isAnalog, value, time_stamp))

        conn.commit()
        conn.close()

        print(f"[{time_stamp}] {data}")

    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

    return jsonify({"status": "ok"})

@app.route('/')
def root():
    return "hello there my nigga"

if __name__ == "__main__":
    app.run(host='0.0.0.0', port=5000)