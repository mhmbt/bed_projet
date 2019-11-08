# -*- coding: utf-8 -*- 

import serial 
ser = serial.Serial('/dev/ttyACM0')
print("Connected on serial port ", ser.name) 

while(True): 
    s=ser.read(100) 
    print("from card : ", s) 
ser.close() 
