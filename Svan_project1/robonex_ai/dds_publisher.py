import json
import random
import numpy as np
from datetime import datetime, timezone, timedelta
from cyclonedds.domain import DomainParticipant
from cyclonedds.topic import Topic
from cyclonedds.pub import DataWriter
from cyclonedds.idl import IdlStruct
from cyclonedds.idl.types import uint8, float32, array
from dataclasses import dataclass

@dataclass
class JoyData_(IdlStruct, typename="xterra::msg::dds_::JoyData_"):
    priority: uint8
    axes:    array[float32, 6]
    buttons: array[uint8,  12]

class DDSPublisher:
    def __init__(self, topic_name="rt/experiment/joystick_data"):
        self.participant = DomainParticipant()
        self.topic = Topic(self.participant, topic_name, JoyData_)
        self.writer = DataWriter(self.participant, self.topic)
        
        # Tracking the whole signature instead of just mode
        self.last_signature = None
        self.button_hold_ticks = 0
        
        print(f" DDS Publisher initialized on topic: '{topic_name}'")

    def publish_movement(self, state_dict):
        mode = state_dict.get("mode", 1)
        vx = round(float(state_dict.get("x_velocity", 0.0)), 2)
        vy = round(float(state_dict.get("y_velocity", 0.0)), 2)
        wz = round(float(state_dict.get("yaw_velocity", 0.0)), 2)

# Standard ROS to Simulator Translation
        axes = np.zeros(6, dtype=np.float32)
        if mode == 4: 
            # THE INVERSION: AI sends Positive for Forward, but Simulator needs Negative.
            # Notice the minus sign at the front!
            axes[1] = -float(np.clip(vx, -0.5, 0.5)) 
            
            # axes[0] controls Left/Right Strafing 
            axes[0] = -float(np.clip(vy, -0.4, 0.4))
            
            # axes[3] controls Yaw/Turning 
            axes[3] = -float(np.clip(wz, -0.8, 0.8))

        # The "Stale Message" Hack (Keep this!)
        # Injecting invisible micro-noise keeps the connection alive
        axes[5] = random.uniform(-0.0001, 0.0001)

        # Strict Mode Tracking
        # ONLY click the button if the actual mode changes (e.g., Stand -> Move).
        if mode != getattr(self, 'last_mode', -1):
            self.last_mode = mode
            self.button_hold_ticks = 10  # Hold button for 10 frames

        buttons = np.zeros(12, dtype=np.uint8)
        
        # Output the '1' if we are in the middle of a simulated click
        if self.button_hold_ticks > 0:
            if mode == 0:
                buttons[0] = 1 
            elif mode == 1:
                buttons[1] = 1 
            elif mode == 4:
                buttons[2] = 1 
            
            self.button_hold_ticks -= 1  # Count down

        self.writer.write(JoyData_(
            priority=50,
            axes=axes.tolist(),
            buttons=buttons.tolist()
        ))
        
        # --- FIXED LOGGING ---
        # 1. Check if the signature changed (prevents spam while moving continuously)
        # 2. Check if velocity > 0.01 (prevents spam while standing still)
        # Only log if there is actual movement to report
        if abs(vx) > 0.01 or abs(vy) > 0.01 or abs(wz) > 0.01:
            current_signature = f"{mode}_{vx:.4f}_{vy:.4f}_{wz:.4f}"
            
            if getattr(self, 'last_print_signature', None) != current_signature:
                ist_offset = timezone(timedelta(hours=5, minutes=30))
                ist_time = datetime.now(ist_offset).strftime('%d-%m-%Y %I:%M:%S %p')
                
                # print("\n--- NEW COMMAND RECEIVED ---")
                # print(f"Time: {ist_time}")
                # print(f"Y-Velocity (Left/Right): {vy}")
                # print(f"X-Velocity (Fwd/Back):   {vx}")
                # print(f"Yaw-Velocity (Turn):     {wz}")
                
                self.last_print_signature = current_signature


# Create the singleton instance
robot_publisher = DDSPublisher()
