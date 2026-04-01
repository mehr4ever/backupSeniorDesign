import speech_recognition as sr
import rospy
from std_msgs.msg import String

def voice_command_node():
    # 1. Initialize the ROS node on your laptop
    # Replace 'duckiebot_name' with your actual bot's name (e.g., 'db21')
    rospy.init_node('voice_commander', anonymous=True)
    pub = rospy.Publisher('/duckiebot_name/voice_commands', String, queue_size=10)
    
    recognizer = sr.Recognizer()
    microphone = sr.Microphone()

    print("--- Voice Control Active: Say 'Forward', 'Stop', or 'Left' ---")

    while not rospy.is_shutdown():
        with microphone as source:
            recognizer.adjust_for_ambient_noise(source)
            print("Listening...")
            try:
                audio = recognizer.listen(source, timeout=5)
                # Convert speech to text
                command = recognizer.recognize_google(audio).lower()
                print(f"I heard: {command}")
                
                # Publish the command to the Duckiebot
                pub.publish(command)
                
            except sr.UnknownValueError:
                print("Could not understand audio")
            except sr.RequestError:
                print("Could not request results; check your internet")
            except Exception as e:
                print(f"Error: {e}")

if __name__ == '__main__':
    try:
        voice_command_node()
    except rospy.ROSInterruptException:
        pass
