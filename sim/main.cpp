#include "Deque.hpp"
#include "CPU.hpp"
#include <iostream>
#include <string>


//main loop here
int main(){

    std::string cmd;
    int int1;

    CPU CPU;
  
    while (std::cin >> cmd){

        if (cmd == "ON"){
            std::cin >> int1;
            CPU.ON(int1);
        }
        else if (cmd == "SPAWN"){
            std::cin >> int1;
            CPU.SPAWN(int1);   
        }
        else if (cmd == "RUN"){
            std::cin >> int1;
            CPU.RUN(int1);
        }
        else if (cmd == "SLEEP"){
            std::cin >> int1;
            CPU.SLEEP(int1);
        }
        else if (cmd == "SHUTDOWN"){
            CPU.SHUTDOWN();
        }
        else if (cmd == "SIZE"){
            std::cin >> int1;
            CPU.SIZE(int1);
        }
        else if (cmd == "CAPACITY"){
                std::cin >> int1;
                CPU.CAPACITY(int1);
            }
        else if (cmd == "EXIT"){
            break;
        }
    }
    return 0;
}