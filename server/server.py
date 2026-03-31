from flask import Flask, request, jsonify, render_template
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
    return render_template("dashboard.html")

@app.route('/latest', methods=['GET'])
def latest_data():
    limit = request.args.get('limit', default=10, type=int)

    # enforce bounds
    if limit < 1:
        limit = 1
    elif limit > 100:
        limit = 100
    
    name = request.args.get('name')  # optional filter

    conn = sqlite3.connect(DB_NAME, timeout=5)
    c = conn.cursor()

    if name:
        c.execute('''
            SELECT name, type, isAnalog, value, timestamp
            FROM sensor_data
            WHERE name = ?
            ORDER BY id DESC
            LIMIT ?
        ''', (name, limit))
    else:
        c.execute('''
            SELECT name, type, isAnalog, value, timestamp
            FROM sensor_data
            ORDER BY id DESC
            LIMIT ?
        ''', (limit,))

    rows = c.fetchall()
    conn.close()

    json = []
    for r in rows:
        json.append({
            "name": r[0],
            "type": r[1],
            "isAnalog": bool(r[2]),
            "value": r[3],
            "timestamp": r[4]
        })

    return jsonify(json)

@app.route('/names', methods=['GET'])
def get_names():
    conn = sqlite3.connect(DB_NAME, timeout=5)
    c = conn.cursor()

    c.execute('SELECT DISTINCT name FROM sensor_data')
    rows = c.fetchall()
    conn.close()

    names = [r[0] for r in rows]

    return jsonify(names)

if __name__ == "__main__":
    app.run(host='0.0.0.0', port=5000)