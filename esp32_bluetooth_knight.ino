#include "BluetoothSerial.h"
BluetoothSerial SerialBT;

void setup() {
  SerialBT.begin("Knight"); // Nombre del dispositivo Bluetooth
  Serial2.begin(115200);      // Inicializa comunicación serial
}

void loop() {
  if (SerialBT.available() > 0) {
    int datoBT = SerialBT.read();  // Lee dato recibido por Bluetooth
    Serial2.write(datoBT);         // Envía el dato por Serial2
  }
}
