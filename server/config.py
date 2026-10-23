gemini_api_key = "YOUR_GEMINI_API_KEY_HERE"

emotion_base = "base"
emotions = ["excited", "delighted", "happy", "content", "relaxed", "calm", "tired", "bored", "depressed", "frustrated", "angry", "tense"]

isLargerScreen = False

# Used for a device until its timezone has been looked up from the IP it connects from.
default_timezone = "America/New_York"

pixel_art_ratio = "1:1"

SENSOR_LABELS = {
    "temperature": "Air Temperature",
    "humidity":    "Air Humidity",
    "sound":       "Sound Level",
    "water":       "Water / Moisture Content",
    "light":       "Light Intensity",
    "motion":      "Object Motion",
    "occupancy":   "Environment Occupancy",
}

SENSOR_METADATA = {
    "temperature": {
        "unit": "°C",
        "sensor value description": "Ambient air temperature.",
    },
    "humidity": {
        "unit": "% RH",
        "sensor value description": "Relative humidity.",
    },
    "sound": {
        "unit": "dBFS",
        "sensor value description": "Sound level. Silence/Noise Floor (-60 dBFS and below), Quiet (-40 to -60 dBFS), Dialogue or music (-40 to -20 dBFS), Loud (-20 to 0 dBFS)",
    },
    "water": {
        "unit": "%",
        "sensor value description": "Water moisture (sensor often used for soil moisture sensing). 100% means immersed in water. 0% means totally dry, like held in the air. Values in between suggest partial moisture.",
        "min": 3154,
        "max": 960,
    },
    "light": {
        "unit": "lux",
        "sensor value description": "Light intensity. Higher values indicate brighter light exposure. Dark is around 7 lux; office lighing is around 127 lux; next to a window in the day is around 600 lux; full sun is often above 2000 lux.",
    },
    "motion": {
        "unit": "%",
        "sensor value description": "Motion of the device as a percentage. 100% indicates being in motion, while 0% means no motion. For historical data, motion is sampled every 2 seconds and aggregated by hour, so values in between 0 and 100 show what % of 2-second samples in that hour there was motion.",
        "min": 0,
        "max": 10,
    },
    "occupancy": {
        "unit": "%",
        "sensor value description": "Indicates human occupancy near the device over time. 100% indicates human occupancy near the device, while 0% indicates no people around the device. For historical data, motion is sampled every 2 seconds and aggregated by hour, so values in between 0 and 100 show what % of 2-second samples in that hour there was human occupancy.",
        "min": 0,
        "max": 3000,
    },
}

