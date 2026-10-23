import json
from pathlib import Path
from google import genai
from google.genai import types
from config import *
from PIL import Image
from io import BytesIO
import numpy as np
import time
import random


# ---------------------------------------------------------------------------
# Prompts
# ---------------------------------------------------------------------------

def get_base_character_prompt(object_desp: str) -> str:
    return f"""
Create a pixel art character based on this object: {object_desp}

The character should be an expressive avatar that can show different emotions.

## Output format
- The character should be drawn to fill the image area as much as possible
- White background
- Black and white color scheme only
- No text in the image
- Simple, clean pixel art style
- The character should have a neutral expression

Generate the base character now.
"""

def get_emotion_prompt(emotion: str) -> str:
    return f"""
Now generate the SAME character showing the emotion: {emotion}
IMPORTANT: Keep the exact same character design, style, and proportions. Only change the facial expression and body language to show {emotion}.

## Output format
- The character should be drawn to fill the image area as much as possible
- White background
- Black and white color scheme only
- No text in the image
- Same pixel art style as before
"""

def get_animation_prompt(emotion: str, frame_idx: int, frame_num: int) -> str:
    return f"""
Generate frame {frame_idx + 1} of {frame_num} in a looping idle animation of a character showing the emotion: {emotion}.
* Input: the previous frame image.
* Output: the next frame with only subtle motion changes (e.g., slight body bob, breathing, blinking).
* The facial expression and emotional state MUST stay {emotion} throughout every frame — do NOT drift or transition to any other emotion.

IMPORTANT: Keep the exact same character design, style, and proportions as the input image. Only add minimal idle motion.

## Output format
- The character should be drawn to fill the image area as much as possible
- White background
- Black and white color scheme only
- No text in the image
"""

def get_determine_emotion_prompt(sensor_data: dict, object_desp: str, positive: str, negative: str, emotions_str: str, current_datetime: str = "") -> str:
    sensor_json = json.dumps(sensor_data, indent=2)
    datetime_line = f"Current date and time: {current_datetime}\n\n" if current_datetime else ""
    return (
        f"Object: {object_desp}\n\n"
        f"{datetime_line}"
        f"Sensors:\n{sensor_json}\n\n"
        f"Positive behaviors (good state): {positive}\n"
        f"Negative behaviors (bad state): {negative}\n"
        f"Available emotions: {emotions_str}\n\n"
        f"Use both the current reading and historical trend (sampled hourly, based on 2-second samples aggregated into hourly averages) to choose exactly ONE emotion "
        f"that best matches the sensor state. Consider the current time of day and date for context. Briefly explain why.\n\n"
        f"Reply in exactly this format (two lines, nothing else):\n"
        f"EMOTION: <emotion>\n"
        f"REASON: <one sentence explanation>"
    )

# def get_emotion_logic_prompt(object_name: str, positive: str, negative: str, emotions_str: str) -> str:
#     return (
#         f"Object: {object_name}\n"
#         f"Positive state (good sensor conditions): {positive}\n"
#         f"Negative state (bad sensor conditions): {negative}\n\n"
#         f"Generate emotion mapping rules for an ADC voltage reading (0–10 V).\n"
#         f"Available emotions: {emotions_str}\n\n"
#         f"Output format — one rule per line:\n"
#         f"  voltage OP THRESHOLD EMOTION\n"
#         f"Where OP is one of: <  <=  >  >=\n"
#         f"Last line must be: default EMOTION\n"
#         f"Rules are evaluated top to bottom; first match wins.\n"
#         f"Example:\n"
#         f"  voltage < 2.0 excited\n"
#         f"  voltage < 5.0 content\n"
#         f"  voltage < 8.0 tired\n"
#         f"  default depressed\n\n"
#         f"Output ONLY the rules. No comments, no explanation."
#     )

def get_positive_attributes_prompt(object_name: str, sensors_str: str) -> str:
    return (
        f"Object: {object_name}.\n"
        f"Available Sensors: [{sensors_str}].\n"
        f"What human behaviors or environmental changes make this object function optimally or 'feel positive'? "
        f"Explain this strictly through the lens of what the available sensors would detect. "
        f"Do not use rhetoric. Try to be concise. Provide plain text within exactly three sentences."
    )

def get_negative_attributes_prompt(object_name: str, sensors_str: str) -> str:
    return (
        f"Object: {object_name}.\n"
        f"Available Sensors: [{sensors_str}].\n"
        f"What human behaviors or environmental changes make this object function poorly, degrade, or 'feel negative'? "
        f"Explain this strictly through the lens of what the available sensors would detect. "
        f"Do not use rhetoric. Try to be concise. Provide plain text within exactly three sentences."
    )


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _png_bytes_to_bin(png_data: bytes) -> bytes:
    """Convert raw PNG bytes to 1-bit packed bitmap with 8-byte header (200x200)."""
    img = Image.open(BytesIO(png_data)).resize((200, 200)).convert("1")
    width, height = img.size
    pixels = img.tobytes()
    header = width.to_bytes(2, "big") + height.to_bytes(2, "big") + b"\x00\x00\x00\x00"
    return header + pixels


def _save_image(png_data: bytes, png_path: Path, bin_path: Path):
    """Save PNG bytes to both .png and .bin paths, creating parent dirs as needed."""
    png_path.parent.mkdir(parents=True, exist_ok=True)
    bin_path.parent.mkdir(parents=True, exist_ok=True)
    png_path.write_bytes(png_data)
    bin_path.write_bytes(_png_bytes_to_bin(png_data))


def _new_image_chat(client):
    return client.chats.create(
        model="gemini-2.5-flash-image",
        config=types.GenerateContentConfig(
            response_modalities=["IMAGE", "TEXT"],
            image_config=types.ImageConfig(aspect_ratio=pixel_art_ratio),
        ),
    )


# ---------------------------------------------------------------------------
# Avatar generation — split into two stages
# ---------------------------------------------------------------------------

def create_base_avatar(user_dir: Path, object_desp: str):
    """Stage 1 (avatar page): Generate the base character and save to
    user_dir/png/base.png and user_dir/bin/base.bin.
    """
    png_path = user_dir / "png" / "base.png"
    bin_path = user_dir / "bin" / "base.bin"

    client = genai.Client(api_key=gemini_api_key)
    chat = _new_image_chat(client)

    prompt = get_base_character_prompt(object_desp)
    print(f"Generating base character for '{object_desp}'...")
    response = chat.send_message(prompt)

    for part in response.candidates[0].content.parts:
        if part.inline_data is not None:
            _save_image(part.inline_data.data, png_path, bin_path)
            print(f"Base character saved: {png_path}, {bin_path}")
            return

    raise RuntimeError("Gemini returned no image for base character")


ANIMATION_FRAMES = 4


def create_all_emotions(user_dir: Path, progress_callback=None, file_saved_callback=None):
    """Stage 2 (behaviors page Done): Generate all emotion stills + animation
    frames using the existing base.png to maintain character consistency.

    Saves PNGs to user_dir/png/<emotion>/N.png
    Saves bins to user_dir/bin/<emotion>/N.bin

    progress_callback(current: int, total: int, message: str) is called after
    each completed step so the server can report progress to the browser.
    """
    base_png_path = user_dir / "png" / "base.png"
    base_bin_path = user_dir / "bin" / "base.bin"
    if not base_bin_path.exists() or not base_png_path.exists():
        print(f"Base image not found at {base_bin_path} — skipping emotion generation")
        return

    total_steps = len(emotions) * (1 + ANIMATION_FRAMES)  # still + frames per emotion
    current_step = 0

    def _report(message: str):
        nonlocal current_step
        current_step += 1
        print(f"  [{current_step}/{total_steps}] {message}")
        if progress_callback:
            progress_callback(current_step, total_steps, message)

    client = genai.Client(api_key=gemini_api_key)

    # Start a fresh chat and send the base image to re-establish character context
    chat = _new_image_chat(client)
    base_img = Image.open(base_png_path).convert("RGBA")
    chat.send_message([
        "This is the base character design. Keep this exact character in all future images.",
        base_img,
    ])

    # Generate each emotion still — skip if already exists
    for emotion in emotions + ["curious"]:
        still_png = user_dir / "png" / emotion / "0.png"
        still_bin = user_dir / "bin" / emotion / "0.bin"
        if still_png.exists() and still_bin.exists():
            _report(f"Skipped {emotion} still (exists)")
            continue
        response = chat.send_message(get_emotion_prompt(emotion))
        for part in response.candidates[0].content.parts:
            if part.inline_data is not None:
                _save_image(part.inline_data.data, still_png, still_bin)
                if file_saved_callback:
                    file_saved_callback("-".join(still_bin.relative_to(user_dir).parts))
                _report(f"Generated {emotion}")
                break

    # Generate animation frames per emotion — skip individual frames that already exist.
    # Each frame is self-contained (prompt + previous frame image), so skipping earlier
    # frames only means those are loaded from disk instead of from a Gemini response.
    for emotion in emotions:
        anim_chat = _new_image_chat(client)
        img = Image.open(user_dir / "png" / emotion / "0.png").convert("RGBA")
        for frame_idx in range(ANIMATION_FRAMES):
            frame_png = user_dir / "png" / emotion / f"{frame_idx + 1}.png"
            frame_bin = user_dir / "bin" / emotion / f"{frame_idx + 1}.bin"
            if frame_png.exists() and frame_bin.exists():
                img = Image.open(frame_png).convert("RGBA")
                _report(f"Skipped {emotion} frame {frame_idx + 1} (exists)")
                continue
            prompt = get_animation_prompt(emotion, frame_idx, ANIMATION_FRAMES)
            response = anim_chat.send_message([prompt, img])
            for part in response.candidates[0].content.parts:
                if part.inline_data is not None:
                    raw = part.inline_data.data
                    _save_image(raw, frame_png, frame_bin)
                    if file_saved_callback:
                        file_saved_callback("-".join(frame_bin.relative_to(user_dir).parts))
                    _report(f"Animated {emotion} frame {frame_idx + 1}/{ANIMATION_FRAMES}")
                    img = Image.open(BytesIO(raw)).convert("RGBA")
                    break

    print("Done! All emotion images generated.")


# ---------------------------------------------------------------------------
# Emotion determination (Method 2 — called per sensor reading)
# ---------------------------------------------------------------------------

def determine_emotion(object_desp: str, positive: str, negative: str, sensor_values: dict, current_datetime: str = "") -> tuple[str, str]:
    # time.sleep(5)
    # return "tense", "test emotion without actual inference"
    """Pick one emotion from the list based on live sensor readings and behaviors.

    Returns:
        (emotion, reason) — emotion is one of the available emotion strings,
        reason is a short explanation of why that emotion was chosen.
    """
    # Enrich sensor data with unit and description from config before sending to Gemini
    enriched = {}
    for name, data in sensor_values.items():
        entry = dict(data)
        meta = SENSOR_METADATA.get(name, {})
        if meta:
            entry["unit"] = meta["unit"]
            entry["sensor value description"] = meta["sensor value description"]

            if meta.get("min", None) is None or meta.get("max", None) is None:
                pass
            else:
                entry["current value"] = (entry["current value"]-meta["min"])/(meta["max"]-meta["min"])
                entry["history"] = (np.array(entry["history"])-meta["min"])/(meta["max"]-meta["min"])
                if entry["unit"] == "%":
                    # For binary-like sensors (motion, occupancy), snap current value to 0 or 100
                    if name in ("motion", "occupancy"):
                        entry["current value"] = 100.0 if entry["current value"] >= 0.5 else 0.0
                    else:
                        entry["current value"] = np.round(entry["current value"]*100,1)
                    entry["history"] = np.round(entry["history"]*100,1)
                else:
                    entry["current value"] = int(np.round(entry["current value"],0))
                    entry["history"] = np.round(entry["history"],0).astype(int)
                entry["history"] = entry["history"].tolist()
        enriched[name] = entry

    client = genai.Client(api_key=gemini_api_key)
    emotions_str = ", ".join(emotions)
    prompt = get_determine_emotion_prompt(enriched, object_desp, positive, negative, emotions_str, current_datetime)
    print(prompt)
    response = client.models.generate_content(
        model="gemini-3-flash-preview",
        config=types.GenerateContentConfig(temperature=0.3),
        contents=prompt,
    )
    emotion = "base"
    reason = ""
    for line in response.text.strip().splitlines():
        if line.startswith("EMOTION:"):
            parsed = line[len("EMOTION:"):].strip().lower()
            if parsed in emotions:
                emotion = parsed
        elif line.startswith("REASON:"):
            reason = line[len("REASON:"):].strip()
    return emotion, reason


# ---------------------------------------------------------------------------
# Emotion logic file generation (Method 1 — downloaded to device)
# ---------------------------------------------------------------------------

def generate_emotion_logic(object_name: str, positive: str, negative: str) -> str:
    """Generate a threshold rules file (emotion_logic.txt) for on-device evaluation.

    File format:
        voltage OP THRESHOLD EMOTION   # e.g. voltage < 3.0 happy
        default EMOTION
    Rules are evaluated top-to-bottom; first match wins.
    """
    client = genai.Client(api_key=gemini_api_key)
    emotions_str = ", ".join(emotions)
    prompt = get_emotion_logic_prompt(object_name, positive, negative, emotions_str)
    response = client.models.generate_content(
        model="gemini-3-flash-preview",
        config=types.GenerateContentConfig(temperature=0.5),
        contents=prompt,
    )
    return response.text.strip()


# ---------------------------------------------------------------------------
# Behavior defaults generation
# ---------------------------------------------------------------------------

def get_positive_attributes(object_name, sensors_list):
    client = genai.Client(api_key=gemini_api_key)

    sensors_str = ", ".join(sensors_list)
    prompt_text = get_positive_attributes_prompt(object_name, sensors_str)

    response = client.models.generate_content(
        model="gemini-3-flash-preview",
        config=types.GenerateContentConfig(
            system_instruction=(
                "You are an assistant that maps positive environmental and human interaction states related to specific sensor capability. "
                "Always respond in exactly three concise sentences.\n\n"
                "Example answer when Object is 'Sea Onion Cactus' and Sensors are ['Temperature', 'Humidity', 'Water / Moisture', 'Light Intensity']:\n"
                "Temperature warm and moderate is good, ideally between 50°F and 75°F (10°C to 24°C). "
                "Low to average household humidity, 30% to 50%, is ideal. Do not need to mist this plant."
                "A soil moisture sensor detecting a brief spike in water followed by a long dry period indicates proper watering behavior. "
                "Bright, indirect light to partial sun is good."
            ),
            temperature=0.7
        ),
        contents=prompt_text
    )

    return response.text


def get_negative_attributes(object_name, sensors_list):
    client = genai.Client(api_key=gemini_api_key)

    sensors_str = ", ".join(sensors_list)
    prompt_text = get_negative_attributes_prompt(object_name, sensors_str)

    response = client.models.generate_content(
        model="gemini-3-flash-preview",
        config=types.GenerateContentConfig(
            system_instruction=(
                "You are an assistant that maps negative environmental and human interaction states related to specific sensor capability. "
                "Always respond in exactly three concise sentences.\n\n"
                "Example answer when Object is 'Sea Onion Cactus' and Sensors are ['Temperature', 'Humidity', 'Water / Moisture', 'Light Intensity']:\n"
                "A temperature sensor dropping below 40°F (4°C) combined with prolonged high humidity indicates a freezing, rot-prone environment. "
                "A water moisture sensor detecting constant, saturated soil levels reveals harmful overwatering behavior that drowns the roots. "
                "A light intensity sensor registering continuous low lux or total darkness confirms the plant is being starved of necessary energy."
            ),
            temperature=0.7
        ),
        contents=prompt_text
    )

    return response.text

if __name__ == "__main__":
    import json
    config         = json.load(open("user/0F6C3C/config.json", 'r'))
    behavior       = json.load(open("user/0F6C3C/behaviors.json", 'r'))
    sensor_history = json.load(open("user/0F6C3C/sensor_history_simulated.json", 'r'))
    emotion, reason = determine_emotion(
        object_desp   = config['desp'],
        positive      = behavior['positive'],
        negative      = behavior['negative'],
        sensor_values = sensor_history["sensors"],
    )
    print(f"Emotion: {emotion}")
    print(f"Reason:  {reason}")
