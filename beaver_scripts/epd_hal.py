import hw_conf
from machine import Pin, SPI
import framebuf
import utime




""" COMMANDS
Explained in Datasheet on page 28
https://static.chipdip.ru/lib/068/DOC043068545.pdf
"""

DRIVER_OUTPUT_CONTROL = 0x01
GATE_DRIVING_VOLTAGE_SOURCE = 0x03
SOURCE_DRIVING_VOLTAGE_SOURCE = 0x04
INITIAL_CODE_SETTING_OTP_PROGRAM = 0x08
WRITE_REGISTER_FOR_INITIAL_CODE_SETTING = 0x09
READ_REGISTER_FOR_INITIAL_CODE_SETTING = 0x0A
BOOSTER_SOFT_START_CONTROL = 0x0C
DEEP_SLEEP_MODE = 0x10
RAM_DATA_ENTRY_MODE = 0x11
SW_RST = 0x12
TEMPERATURE_SENSOR_CONTROL_SEL = 0x18
TEMPERATURE_SENSOR_CONTROL_WRITE = 0x1A
MASTER_ACTIVATION = 0x20
DISPLAY_UPDATE_CONTROL_1 = 0x21
DISPLAY_UPDATE_CONTROL_2 = 0x22
WRITE_RAM_BLACK_WHITE = 0x24
WRITE_RAM_RED_WHITE = 0x26
WRITE_VCOM_REGISTER = 0x2C
OTP_REGISTER_READ = 0x2D
STATUS_REGISTER_READ = 0x2F
PROGRAM_WS_OTP = 0x30
WRITE_LUT_REGISTER = 0x32
OTP_PROGRAM_MODE = 0x39
SET_RAM_X_ADDRESS_POS = 0x44
SET_RAM_Y_ADDRESS_POS = 0x45
SET_RAM_X_ADDRESS_CNT = 0x4E
SET_RAM_Y_ADDRESS_CNT = 0x4F



WF_PARTIAL_2IN66 =[
0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x80,0x80,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x40,0x40,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x80,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x0A,0x00,0x00,0x00,0x00,0x00,0x02,0x01,0x00,0x00,
0x00,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,0x22,0x22,0x22,0x22,0x22,0x22,
0x00,0x00,0x00,0x22,0x17,0x41,0xB0,0x32,0x36,
]

class EPD_2in66:
    def __init__(self):
        self.reset_pin = Pin(hw_conf.RST_PIN, Pin.OUT)
        self.busy_pin = Pin(hw_conf.BUSY_PIN, Pin.IN, Pin.PULL_UP)
        self.cs_pin = Pin(hw_conf.CS_PIN, Pin.OUT)
        self.width = hw_conf.EPD_WIDTH
        self.height = hw_conf.EPD_HEIGHT
        self.lut = WF_PARTIAL_2IN66

        self.spi = SPI(1)
        self.spi.init(baudrate=4000000)
        self.dc_pin = Pin(hw_conf.DC_PIN, Pin.OUT)
        
        self.buffer_landscape_black = bytearray(self.height * self.width // 8)
        self.buffer_landscape_red = bytearray(self.height * self.width // 8)
        
        self.image_landscape_black = framebuf.FrameBuffer(self.buffer_landscape_black, self.height, self.width, framebuf.MONO_VLSB)
        self.image_landscape_red = framebuf.FrameBuffer(self.buffer_landscape_red, self.height, self.width, framebuf.MONO_VLSB)
        self.init(0)

    # Hardware reset
    def reset(self):
        self.reset_pin(1)
        utime.sleep_ms(200) 
        self.reset_pin(0)
        utime.sleep_ms(200)
        self.reset_pin(1)
        utime.sleep_ms(200)   

    def send_command(self, command):
        self.cs_pin(1)
        self.dc_pin(0)
        self.cs_pin(0)
        self.spi.write(bytearray([command]))
        self.cs_pin(1)

    def send_data(self, data):
        self.cs_pin(1)
        self.dc_pin(1)
        self.cs_pin(0)
        self.spi.write(bytearray([data]))
        self.cs_pin(1)
        
    def send_data1(self, buf):
        self.cs_pin(1)
        self.dc_pin(1)
        self.cs_pin(0)
        self.spi.write(bytearray(buf))
        self.cs_pin(1)
        
    def ReadBusy(self):
        print('e-Paper busy')
        utime.sleep_ms(100)   
        while(self.busy_pin.value() == 1):      # 0: idle, 1: busy
            utime.sleep_ms(100)    
        print('e-Paper busy release')
        utime.sleep_ms(100)  
    
    def GetDimensions(self):
        return (self.width, self.height)
        
    def TurnOnDisplay(self):
        self.send_command(0x20)        
        self.ReadBusy()
        
    def SendLut(self):
        self.send_command(0x32)
        for i in range(0, 153):
            self.send_data(self.lut[i])
        self.ReadBusy()
    
    def SetWindow(self, x_start, y_start, x_end, y_end):
        self.send_command(0x44) # SET_RAM_X_ADDRESS_START_END_POSITION
        # x point must be the multiple of 8 or the last 3 bits will be ignored
        self.send_data((x_start>>3) & 0xFF)
        self.send_data((x_end>>3) & 0xFF)
        self.send_command(0x45) # SET_RAM_Y_ADDRESS_START_END_POSITION
        self.send_data(y_start & 0xFF)
        self.send_data((y_start >> 8) & 0xFF)
        self.send_data(y_end & 0xFF)
        self.send_data((y_end >> 8) & 0xFF)

    def SetCursor(self, x, y):
        self.send_command(0x4E) # SET_RAM_X_ADDRESS_COUNTER
        self.send_data(x & 0xFF)
        
        self.send_command(0x4F) # SET_RAM_Y_ADDRESS_COUNTER
        self.send_data(y & 0xFF)
        self.send_data((y >> 8) & 0xFF)
    
    def init(self, mode):
        self.reset()
         
        self.send_command(0x12)  #SWRESET
        self.ReadBusy()
        
        self.send_command(0x11) #RAM
        self.send_data(0x03)
        
        self.SetWindow(8, 0, self.width, self.height)
   
        if(mode == 0):
            self.send_command(0x3c)
            self.send_data(0x01)
        elif(mode == 1):
            self.SendLut()
            self.send_command(0x37) # set display option, these setting turn on previous function
            self.send_data(0x00)
            self.send_data(0x00)
            self.send_data(0x00)
            self.send_data(0x00)
            self.send_data(0x00)  
            self.send_data(0x40)
            self.send_data(0x00)
            self.send_data(0x00)
            self.send_data(0x00)
            self.send_data(0x00)

            self.send_command(0x3C)
            self.send_data(0x80)

            self.send_command(0x22)
            self.send_data(0xcf)
            
            self.send_command(0x20)
            self.ReadBusy()
            
        else: 
            print("There is no such mode")
    
    def display(self, image):
        if (image == None):
            return            
            
        self.SetCursor(1, 295)
        
        self.send_command(0x24) # WRITE_RAM
        self.send_data1(image)
        self.TurnOnDisplay()
        
    def display_Landscape(self, image, color):
        if (image == None):
            return   
        if(self.width % 8 == 0):
            Width = self.width // 8
        else:
            Width = self.width // 8# +1

        Height = self.height
        self.SetCursor(1, 295)
        
        self.send_command(color)
        for j in range(Height):
            for i in range(Width):
                self.send_data(image[(Width-i-1) * Height + j])
                
        
    def Clear(self, color):
        self.send_command(0x24) # WRITE_RAM
        self.send_data1([color] * self.height * int(self.width / 8))

        self.send_command(0x26)
        self.send_data1([~color] * self.height * int(self.width / 8))

        self.TurnOnDisplay()
    
    def sleep(self):
        self.send_command(0x10) # DEEP_SLEEP_MODE
        self.send_data(0x01)