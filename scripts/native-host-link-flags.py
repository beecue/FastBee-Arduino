import platform

Import("env")

if platform.system() == "Windows":
    env.Append(LIBS=["ws2_32"])
