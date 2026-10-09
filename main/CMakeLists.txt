idf_component_register(
                        SRCS 
                                "webserver.c" "tcp_server.c" "uart.c" "save_nvs.c" "main.c"
                                "wifi_manager.c" "mqtt_client.c" "modbus.c" "webserver.h"
                                
                        INCLUDE_DIRS "."
                                        
                                
                        EMBED_TXTFILES
                                "index.html"
                   
                      )
