#!/usr/bin/env python

# Request register values:
GET_BRICK_COLOR_WAIT = 0
GET_BRICK_COLOR_RED = 1
GET_BRICK_COLOR_YELLOW = 2 
GET_BRICK_COLOR_BLUE = 3

# Result register values:
RESULT_WAIT = 99
RESULT_COLOR_MATCH = 1
RESULT_COLOR_MISMATCH = 0

# Color Values
RED_BRICK = "red"
YELLOW_BRICK = "yellow"
BLUE_BRICK = "blue"

# Configuration parameters
UNIT = 0x1
ADDRESS = 0x10
VISUALIZER_REQUEST_RATE = 0.1


# SERVER PARAMETERS
IP = '192.168.0.4'
PORT = 5020

REGISTER_REFRESH_FREQUENCY = 1