import time
import asyncio
import speech_recognition as sr
from brain import process_text_command

def mic_listen_loop():
    recognizer = sr.Recognizer()
    
    # --- THE FIXES ---
    # 1. Increased to 0.8s: Gives you enough time to pause between phrases without cutting you off.
    recognizer.pause_threshold = 0.8
    
    # 2. Increased to 0.5s: Prevents the mic from aggressively deleting quiet "S" sounds at the start of words.
    recognizer.non_speaking_duration = 0.5
    
    # 3. TURN OFF DYNAMIC THRESHOLD: This locks the volume gate. It stops the mic from 
    # wandering up and accidentally classifying your quiet consonants as "background fan noise".
    recognizer.dynamic_energy_threshold = True
    
    # Create a permanent asynchronous engine for this thread
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)

    with sr.Microphone() as source:
        print("\nCalibrating background noise (Please stay quiet for 2 seconds)...")
        recognizer.adjust_for_ambient_noise(source, duration=2)
        
        # We take the perfect room baseline it just learned, and manually add a tiny buffer 
        # to ignore background hums, but keep it locked forever.
        recognizer.energy_threshold += 150 
        
        print(f"Microphone is now continuously listening! (Locked Energy Threshold: {recognizer.energy_threshold})")
        
        while True:
            try:
                # FIX 4: Changed timeout back to 1. (timeout=0 was causing erratic skipping).
                # Increased phrase_time_limit to 8 seconds so you can give longer, complex commands.
                audio = recognizer.listen(source, timeout=1, phrase_time_limit=5)
                
                stt_start = time.time()
                text = recognizer.recognize_google(audio)
                stt_end = time.time()
                
                print(f"\n[STT Latency: {(stt_end - stt_start):.3f}s] You said: '{text}'")
                
                # Run the brain
                loop.run_until_complete(process_text_command(text))
                
            except sr.WaitTimeoutError:
                # Normal behavior: No one spoke, loop silently restarts
                continue 
            except sr.UnknownValueError:
                # Normal behavior: Google heard a chair squeak, but no actual words.
                continue
            except Exception as e:
                print(f"Microphone glitch: {e}")
                time.sleep(1)