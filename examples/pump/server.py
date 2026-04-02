#!/usr/bin/env python3
"""Fake insulin pump REST API.

Simulates a continuous glucose monitor + insulin pump sending readings.
Serves the web UI and provides /api/reading endpoint.

Usage: python3 examples/pump/server.py
Then open http://localhost:8088
"""

import json
import math
import random
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
import os

# Simulated pump state
class PumpSimulator:
    def __init__(self):
        self.t = 0
        self.glucose = 120.0  # mg/dL
        self.iob = 0.0        # insulin on board (units)
        self.last_bolus_t = -999

    def tick(self):
        self.t += 5  # 5-minute intervals

        # Meal spikes (every ~60 readings = 5 hours)
        if self.t % 300 < 5:
            meal_carbs = random.uniform(20, 80)
            self.glucose += meal_carbs * 1.5
            # Auto-bolus
            bolus = round(meal_carbs / 15 * 2) / 2  # round to 0.5 units
            self.iob += bolus
            self.last_bolus_t = self.t

        # IOB decay (exponential, ~4hr half-life)
        self.iob *= 0.97

        # Insulin lowers glucose
        self.glucose -= self.iob * 3

        # Natural drift back to baseline
        self.glucose += (100 - self.glucose) * 0.02

        # Random noise
        self.glucose += random.gauss(0, 3)
        self.glucose = max(40, min(400, self.glucose))

        return {
            "glucose": f"{self.glucose:.1f}M",
            "iob": f"{self.iob:.2f}M",
            "timestamp": int(time.time()),
            "trend": "rising" if self.glucose > 150 else "falling" if self.glucose < 80 else "stable"
        }

pump = PumpSimulator()
readings_buffer = []

# Pre-fill with 2 hours of history
for _ in range(24):
    readings_buffer.append(pump.tick())

class PumpHandler(SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        # Serve from the pump example directory
        super().__init__(*args, directory=os.path.dirname(os.path.abspath(__file__)), **kwargs)

    def do_GET(self):
        if self.path == '/api/reading':
            reading = pump.tick()
            readings_buffer.append(reading)
            if len(readings_buffer) > 288:  # 24 hours at 5min intervals
                readings_buffer.pop(0)
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps(reading).encode())
        elif self.path == '/api/history':
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()
            self.wfile.write(json.dumps(readings_buffer).encode())
        else:
            super().do_GET()

    def log_message(self, format, *args):
        if '/api/' not in str(args[0]):
            super().log_message(format, *args)

if __name__ == '__main__':
    port = 8088
    server = HTTPServer(('', port), PumpHandler)
    print(f"Insulin pump simulator: http://localhost:{port}")
    print(f"API: http://localhost:{port}/api/reading")
    print(f"History: http://localhost:{port}/api/history")
    server.serve_forever()
