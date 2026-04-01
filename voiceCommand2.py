#!/usr/bin/env python3
import speech_recognition as sr
import rospy
from duckietown_msgs.msg import Twist2DStamped

pub = None

def send_move(v, omega):
    msg = Twist2DStamped()
    msg.v = v
    msg.omega = omega
    pub.publish(msg)
    print(f">> Command sent: v={v}, omega={omega}")

def listen_and_command():
    r = sr.Recognizer()
    mic = sr.Microphone(device_index=0)

    with mic as source:
        print("Voice control ready.")
        print("Say: forward, stop, left, right, exit")
        r.adjust_for_ambient_noise(source, duration=1)

        while not rospy.is_shutdown():
            try:
                print("\nListening...")
                audio = r.listen(source, timeout=5, phrase_time_limit=2)
                text = r.recognize_google(audio).lower()
                print(f"Heard: {text}")

                if "forward" in text:
                    send_move(0.4, 0.0)
                elif "stop" in text:
                    send_move(0.0, 0.0)
                elif "left" in text:
                    send_move(0.2, 1.5)
                elif "right" in text:
                    send_move(0.2, -1.5)
                elif "exit" in text:
                    print("Exiting...")
                    break
                else:
                    print("Unknown command")

            except sr.UnknownValueError:
                print("Could not understand audio")
            except sr.WaitTimeoutError:
                print("Listening timed out")
            except Exception as e:
                print(f"Error: {e}")

if __name__ == "__main__":
    rospy.init_node("voice_duckiebot_control")
    pub = rospy.Publisher("/lane_controller_node/car_cmd", Twist2DStamped, queue_size=1)

    rospy.sleep(1)

    try:
        listen_and_command()
    except KeyboardInterrupt:
        pass
    finally:
        send_move(0.0, 0.0)
import time
import speech_recognition as sr
import roslibpy

# --- CONFIGURATION ---
# Replace with your Duckiebot's IP address
DUCKIE_IP = '192.168.1.XXX' 

# Initialize ROS connection
client = roslibpy.Ros(host=DUCKIE_IP, port=9090)

# This topic controls the wheels directly
talker = roslibpy.Topic(client, '/lane_controller_node/car_cmd', 'duckietown_msgs/Twist2DStamped')

def send_move(v, omega):
    """v: linear velocity (0.0 to 1.0), omega: steering (-1.0 to 1.0)"""
    if client.is_connected:
        message = roslibpy.Message({'v': v, 'omega': omega})
        talker.publish(message)
        print(f">> Executing: v={v}, omega={omega}")

def listen_and_command():
    rec = sr.Recognizer()
    mic = sr.Microphone()

    with mic as source:
        print("\n[HP System Active] Ready for commands...")
        print("Commands: 'Forward', 'Stop', 'Left', 'Right'")
        rec.adjust_for_ambient_noise(source, duration=1)
        
        try:
            while True:
                print("Listening...")
                audio = rec.listen(source, phrase_time_limit=2)
                text = rec.recognize_google(audio).lower()
                print(f"Heard: '{text}'")

                if "forward" in text:
                    send_move(0.4, 0.0)
                elif "stop" in text:
                    send_move(0.0, 0.0)
                elif "left" in text:
                    send_move(0.2, 1.0)
                elif "right" in text:
                    send_move(0.2, -1.0)
                elif "exit" in text:
                    break

        except sr.UnknownValueError:
            print("? Could not understand")
        except Exception as e:
            print(f"Error: {e}")

if __name__ == "__main__":
    client.run()
    try:
        listen_and_command()
    except KeyboardInterrupt:
        pass
    finally:
        send_move(0.0, 0.0) # Safety stop
        client.terminate()
