# -*- coding: utf-8 -*- 

import serial 
import re 
ser = serial.Serial('/dev/ttyACM0')
print("Connected on serial port ", ser.name) 

test_message = """0 00 00 
Sent ACK to 0x2A 
nb_nodes : 33 
 id 26 : 2A2A
 id 2A : 22A
 id 2A : 2A
 id 20 : 00

 id 0A : 0A
 id 23 : 123
radio_cb :: rssi -52
 id 00 : 07
 id 03 : DCFF
 id 00 : 00
 id 00 : 43F
 id 00 : 6E
 id 6F : C6E
 id 6F : 6E
 id 6F : 746E
 id 6F : 24FF
 id FFFF : 8
FF
 id FFFF : EA00
 id 00 : F2FF
 id FFFF : 2C0A
 id 0A : 8F0A
 id FF81 : BF02
 id 0A : 7E0A
 id 00 : BE00
 id 41 : 4F04
 id 7E : F7A9
 id 02 : AB00
 id 1F : DF6E
 id 6F : FA6E
 id 6F : D
"""

temp_regex = "\ ?id (.*) : (.*)"
#regexp motor used to parse the temperature messages
prog = re.compile(temp_regex)

##### FUNCTIONS 
# extracts the temperatures from the message read on serial port, returns a dictionary on the format: 
# 'id' : 'temp value'.
def parse_temperatures(message) : 
    temp_dict={}
    lines=splits_message_in_lines(message)
    for line in lines : 
        parse_result = parse_line(line)
        if(bool(parse_result)): 
            temp_dict[parse_result.group(1)] = parse_result.group(2)
    return temp_dict

def parse_line(line): 
    result = prog.search(line)
    return result 
    
# splits the message in lines, and passes it one by one to parse_temperatures()  
def splits_message_in_lines(message):
    lines = message.split("\n", message.count("\n"))
    return lines 

# calculates the average of values in a dictionary by calling calculate_average(), and prints it to standard output. 
def print_average(dictionary) :
    print("AVERAGE : " + str(calculate_average(dictionary)))

# calculate the average of the values in a dictionary. If a value is 0, it is considered undefined and thus not taken into account in the average. 
def calculate_average(dictionary) : 
    average = 0
    nb_values=0
    for id in dictionary : 
        if int(dictionary[id], 16)!= 0 :
            average = average + int(dictionary[id], 16)
            nb_values = nb_values + 1 
    average = average / nb_values
    return average
#####

while(True): 
    #s=ser.read(100) 
    temp_dict = parse_temperatures(test_message) 
    print_average(temp_dict) 

ser.close() 
