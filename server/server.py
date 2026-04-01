from flask import Flask, request, jsonify, render_template
from datetime import datetime
import sqlite3

DB_NAME = "database.db"

# =========================
# INIT DB
# =========================

def init_db():
    conn = sqlite3.connect(DB_NAME)
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

    # 🔥 ADD isAlert
    c.execute('''
        CREATE TABLE IF NOT EXISTS desired_states (
            device TEXT,
            output_id TEXT,
            value INTEGER,
            updated_at TEXT,
            PRIMARY KEY (device, output_id)
        )
    ''')

    # 🔥 ADD isAlert
    c.execute('''
        CREATE TABLE IF NOT EXISTS actual_states (
            device TEXT,
            output_id TEXT,
            value INTEGER,
            isAlert INTEGER,   -- NEW
            updated_at TEXT,
            PRIMARY KEY (device, output_id)
        )
    ''')

    c.execute('''
        CREATE TABLE IF NOT EXISTS overrides (
            device TEXT,
            output_id TEXT,
            enabled INTEGER,
            PRIMARY KEY (device, output_id)
        )
    ''')

    c.execute('''
        CREATE TABLE IF NOT EXISTS outputs (
            device TEXT,
            output_id TEXT,
            isAlert INTEGER,
            last_seen TEXT,
            PRIMARY KEY (device, output_id)
        )
    ''')

    conn.commit()
    conn.close()

init_db()

app = Flask(__name__)

# =========================
# HELPERS
# =========================

def set_desired_state(device, output_id, value):
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    c.execute('''
        INSERT INTO desired_states (device, output_id, value, updated_at)
        VALUES (?, ?, ?, ?)
        ON CONFLICT(device, output_id)
        DO UPDATE SET value = excluded.value,
                      updated_at = excluded.updated_at
    ''', (device, output_id, value, datetime.now()))

    conn.commit()
    conn.close()

def evaluate_rules(device, sensor_type, value, isAnalog):
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    # example rules (hardcoded for now)
    trigger = False

    if isAnalog:
        if sensor_type == "light":
            trigger = (value > 30)

        elif sensor_type == "gas":
            trigger = (value > 400)
            
    else:
        if sensor_type == "light":
            trigger = value>0

        elif sensor_type == "gas":
            trigger = value>0
            
        elif sensor_type == "motion":
            trigger = value>0

    # apply to ALL alert outputs
    c.execute('''
        SELECT output_id FROM outputs
        WHERE device = ? AND isAlert = 1
    ''', (device,))

    rows = c.fetchall()

    for (output_id,) in rows:
        set_desired_state(device, output_id, 1 if trigger else 0)

    conn.close()


# =========================
# DATA ENDPOINT
# =========================

@app.route('/data', methods=['POST'])
def receive_data():
    data = request.get_json()

    if not data:
        return jsonify({"error": "no json"}), 400

    dtype = data.get("type")

    # SENSOR DATA
    if dtype == "sensor":
        device = data.get("device")
        name = data.get("id")
        sensor_type = data.get("sensorType")
        value = float(data.get("value"))
        isAnalog = data.get("isAnalog")

        conn = sqlite3.connect(DB_NAME)
        c = conn.cursor()

        c.execute('''
            INSERT INTO sensor_data (name, type, isAnalog, value, timestamp)
            VALUES (?, ?, ?, ?, ?)
        ''', (name, sensor_type, isAnalog, value, datetime.now()))

        conn.commit()
        conn.close()

        evaluate_rules(device, sensor_type, value, isAnalog)

        return jsonify({"status": "ok"})

    # OUTPUT FEEDBACK
    elif dtype == "output_state":
        device = data.get("device")
        outputs = data.get("outputs", [])

        conn = sqlite3.connect(DB_NAME)
        c = conn.cursor()

        for o in outputs:
            c.execute('''
                INSERT INTO actual_states (device, output_id, value, isAlert, updated_at)
                VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(device, output_id)
                DO UPDATE SET value = excluded.value,
                            isAlert = excluded.isAlert,
                            updated_at = excluded.updated_at
            ''', (device, o["id"], o["value"], int(o.get("isAlert", 0)), datetime.now()))
            
        conn.commit()
        conn.close()

        return jsonify({"status": "ok"})
    
    return jsonify({"error": "invalid type"}), 400

@app.route('/register_outputs', methods=['POST'])
def register_outputs():
    data = request.get_json()
    device  = data.get("device")
    outputs = data.get("outputs", [])

    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    # Rebuild the outputs table so new/removed pins are reflected
    c.execute('DELETE FROM outputs WHERE device = ?', (device,))

    for o in outputs:
        output_id = o["id"]
        isAlert   = int(o.get("isAlert", 0))

        c.execute('''
            INSERT INTO outputs (device, output_id, isAlert, last_seen)
            VALUES (?, ?, ?, ?)
        ''', (device, output_id, isAlert, datetime.now()))

        # INSERT OR IGNORE — keeps existing desired state, only adds if missing
        c.execute('''
            INSERT OR IGNORE INTO desired_states (device, output_id, value, updated_at)
            VALUES (?, ?, 0, ?)
        ''', (device, output_id, datetime.now()))

        # Same for overrides
        c.execute('''
            INSERT OR IGNORE INTO overrides (device, output_id, enabled)
            VALUES (?, ?, 1)
        ''', (device, output_id))

    conn.commit()
    conn.close()
    return jsonify({"status": "synced"})

@app.route('/states')
def get_states():
    device = request.args.get("device")

    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    c.execute('''
        SELECT o.output_id, o.isAlert, d.value, ov.enabled
        FROM outputs o
        LEFT JOIN desired_states d
            ON o.device = d.device AND o.output_id = d.output_id
        LEFT JOIN overrides ov
            ON o.device = ov.device AND o.output_id = ov.output_id
        WHERE o.device = ?
    ''', (device,))

    rows = c.fetchall()
    conn.close()

    result = []

    for output_id, isAlert, value, enabled in rows:
        value = value if value is not None else 0
        enabled = enabled if enabled is not None else 1

        # block alerts if disabled
        if isAlert == 1 and enabled == 0:
            value = 0

        result.append({
            "id": output_id,
            "value": value,
            "enabled": enabled,
            "isAlert": bool(isAlert)
        })

    return jsonify(result)

@app.route('/set_output', methods=['POST'])
def set_output():
    data = request.get_json()
    device    = data.get("device")
    output_id = data.get("id")
    value     = data.get("value")

    # Don't allow the UI to override alert outputs — rules own them
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    c.execute('SELECT isAlert FROM outputs WHERE device = ? AND output_id = ?', (device, output_id))
    row = c.fetchone()
    conn.close()

    if row and row[0] == 1:
        return jsonify({"status": "ignored", "reason": "alert output is rule-controlled"})

    set_desired_state(device, output_id, value)
    return jsonify({"status": "ok"})

# =========================
# DEVICE LIST
# =========================

@app.route('/devices')
def devices():
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    c.execute('SELECT DISTINCT device FROM outputs')
    rows = c.fetchall()

    conn.close()

    return jsonify([r[0] for r in rows])


# =========================
# OVERRIDE CONTROL
# =========================

@app.route('/override', methods=['POST'])
def override():
    data = request.get_json()

    device = data.get("device")
    output_id = data.get("id")
    enabled = data.get("enabled")

    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    c.execute('''
        INSERT INTO overrides (device, output_id, enabled)
        VALUES (?, ?, ?)
        ON CONFLICT(device, output_id)
        DO UPDATE SET enabled = excluded.enabled
    ''', (device, output_id, enabled))

    conn.commit()
    conn.close()

    return jsonify({"status": "ok"})


# =========================
# DEBUG
# =========================

@app.route('/debug')
def debug():
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    c.execute("SELECT * FROM desired_states")
    desired = c.fetchall()

    c.execute("SELECT * FROM actual_states")
    actual = c.fetchall()

    conn.close()

    return jsonify({
        "desired": desired,
        "actual": actual
    })


# =========================
# UI ROUTES
# =========================

@app.route('/')
def root():
    return render_template("dashboard.html")  # unchanged


@app.route('/control')
def control():
    return render_template("control.html")


# =========================
# OLD ROUTES (UNCHANGED)
# =========================

@app.route('/latest')
def latest_data():
    name = request.args.get("name")
    limit = request.args.get("limit", 10)

    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    query = '''
        SELECT name, type, isAnalog, value, timestamp
        FROM sensor_data
    '''

    params = []

    if name:
        query += " WHERE name = ?"
        params.append(name)

    query += " ORDER BY id DESC LIMIT ?"
    params.append(int(limit))

    c.execute(query, params)
    rows = c.fetchall()

    conn.close()

    return jsonify([
        {
            "name": r[0],
            "type": r[1],
            "isAnalog": bool(r[2]),
            "value": r[3],
            "timestamp": r[4]
        }
        for r in rows
    ])
    
@app.route('/set_output_batch', methods=['POST'])
def set_output_batch():
    """Handle multiple output changes at once"""
    data = request.get_json()
    
    device = data.get("device")
    outputs = data.get("outputs", [])
    
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    
    for output in outputs:
        output_id = output.get("id")
        value = output.get("value")
        
        c.execute('''
            INSERT INTO desired_states (device, output_id, value, updated_at)
            VALUES (?, ?, ?, ?)
            ON CONFLICT(device, output_id)
            DO UPDATE SET value = excluded.value,
                          updated_at = excluded.updated_at
        ''', (device, output_id, value, datetime.now()))
    
    conn.commit()
    conn.close()
    
    return jsonify({"status": "ok"})

@app.route('/clear_override', methods=['POST'])
def clear_override():
    """Clear manual override for an output"""
    data = request.get_json()
    device = data.get("device")
    output_id = data.get("id")
    
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()
    
    # Clear override in database
    c.execute('DELETE FROM overrides WHERE device = ? AND output_id = ?', 
              (device, output_id))
    
    conn.commit()
    conn.close()
    
    return jsonify({"status": "ok"})

@app.route('/names')
def names():
    conn = sqlite3.connect(DB_NAME)
    c = conn.cursor()

    c.execute('SELECT DISTINCT name FROM sensor_data')
    rows = c.fetchall()

    conn.close()

    return jsonify([r[0] for r in rows])


# =========================
# RUN
# =========================

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000)