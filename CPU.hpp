#ifndef CPU_HPP
#define CPU_HPP

#include "Deque.hpp"  

class CPU {
private:
    // Dynamically allocate an array of Deques (cores)
    Deque* cores;   
    int N_cores; 

public:
    //constructor and destructor
    CPU();
    ~CPU();

    //helper functions
    int leastBusy() const;
    int mostBusy() const;

    //assigned functions
    void ON(int N);
    void SPAWN(int P_ID);
    void RUN(int C_ID);
    void SLEEP(int C_ID);
    void SHUTDOWN();
    void SIZE(int C_ID);
    void CAPACITY(int C_ID);
    void EXIT();     
};

#endif