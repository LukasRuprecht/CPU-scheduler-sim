#include "CPU.hpp"
#include "Deque.hpp"  
#include <iostream>


// Default constructor
CPU::CPU() : cores(nullptr), N_cores(0) {}

//Destructor and set to nullptr
CPU::~CPU() {
    delete[] cores;
    cores = nullptr;
}

//find core with the least amount of work while prioritizing index if a tie
int CPU::leastBusy() const {

    //start with first core
    int minJobs = cores[0].get_size();  
    int minIndex = 0;

    // loop through all the cores to find the one with the least jobs
    for (int i = 1; i < N_cores; i++) {
        int currentJobs = cores[i].get_size();

        // If the current core has fewer jobs or if there is a tie, pick the lower index
        if (currentJobs < minJobs) {
            minJobs = currentJobs;
            minIndex = i;
            
        } 
        //Prefer lower index
        else if (currentJobs == minJobs && i < minIndex) {
            minIndex = i;  
        }
    }
    
    return minIndex;  // Return the index of the core with the least jobs
}


int CPU::mostBusy() const {
    int maxJobs = cores[0].get_size();  // Start with the first core
    int maxIndex = 0;

    // Loop through all the cores to find the one with the most jobs
    for (int i = 1; i < N_cores; i++) {
        int currentJobs = cores[i].get_size();

        // If the current core has more jobs or if there is a tie, pick the lower index
        if (currentJobs > maxJobs) {
            maxJobs = currentJobs;
            maxIndex = i;
        } else if (currentJobs == maxJobs && i < maxIndex) {
            maxIndex = i;  // Resolve ties by preferring the lower index
        }
    }

    return maxIndex;  // Return the index of the core with the most jobs
}



void CPU::ON(int N) {
    //If CPU already exists
    if (cores != nullptr) {
        std::cout<<"failure"<<std::endl;
        return;
    }
    else{
        cores = new Deque[N]{Deque(4)};
        N_cores = N;

        //Initialize each core with capacity 4
        std::cout<<"success"<<std::endl;
    }
}


void CPU::SPAWN(int P_ID){

    //correct??
    if(P_ID < 1){
        std::cout<<"failure"<<std::endl;
        return;
    }
    //Spawns a new task, 
    int x = leastBusy();

    cores[x].push_b(P_ID); 

    std::cout<<"core "<<x<<" assigned task "<<P_ID<<std::endl;
}


void CPU::RUN(int C_ID){
    //if C_ID out of range
    if (C_ID < 0 || (N_cores-1) < C_ID ){
        std::cout<<"failure"<<std::endl;
        return;
    }

    //This means that this core pops the next task from the front of its queue, if possible
    int task = cores[C_ID].pop_f();

    //task will be -1 if empty
    if (task == -1){
        std::cout<<"core "<<C_ID<<" has no tasks to run"<<std::endl;
    }
   else {
        std::cout<<"core "<<C_ID<<" is running task "<<task<<std::endl;
    } 

    // Only try to steal if there's another core with tasks
    if (cores[C_ID].get_size() == 0){
        int busiest = mostBusy();
        if (cores[busiest].get_size() > 0) {  // Check if there's actually a task to steal
            cores[C_ID].push_b(cores[busiest].pop_b());
        }
    }   
}


void CPU::SLEEP(int C_ID){
    //check bound for C_ID
    if (C_ID < 0 || (N_cores-1) < C_ID ){
        std::cout<<"failure"<<std::endl;
        return;
    }

    //check if empty
    if (cores[C_ID].get_size() == 0){
        std::cout<<"core "<<C_ID<<" has no tasks to assign"<<std::endl;
        return;
    }

    //std::cout<<"SIZE IS "<<cores[C_ID].get_size()<<std::endl;
    //reassign tasks from back of C_ID to back of least busy
    while (cores[C_ID].get_size() > 0){

        int x = cores[C_ID].pop_b();

        int minJobs = cores[0].get_size();  
        int minIndex = 0;

        // loop through all the cores to find the one with the least jobs
        for (int i = 1; i < N_cores; i++) {
            int currentJobs = cores[i].get_size();

            // If the current core has fewer jobs or if there is a tie, pick the lower index
            if (minIndex == C_ID || (currentJobs < minJobs && i != C_ID) ) {
                minJobs = currentJobs;
                minIndex = i;
            } 
            //Prefer lower index
            else if (currentJobs == minJobs && i < minIndex && i != C_ID) {
                minIndex = i;  
            }
        }    
        cores[minIndex].push_b(x);
        std::cout<<"task "<< x << " assigned to core " << minIndex<< " ";
    }
    std::cout<<std::endl;
    
}

void CPU::SHUTDOWN(){
    //case if all cores are empty
    for (int i = 0; i < N_cores; i++) {
        if (cores[i].get_size() != 0) {
            break;  
        }

        // If this is the last core and it is empty
        if (i == N_cores - 1) {
            std::cout << "no tasks to remove" << std::endl;
            return;
        }
    }
    
    //iterate through cores and kill all tasks
    for (int i = 0; i < N_cores; i++){
        while (cores[i].get_size() != 0){
            int x = cores[i].pop_f();
            std::cout<<"deleting task "<< x <<" from core " << i << " ";
        }
    }
    std::cout<<std::endl;
}

void CPU::SIZE(int C_ID){
    if (C_ID < 0 || (N_cores-1) < C_ID ){
        std::cout<<"failure"<<std::endl;
        return;
    }
    int x = cores[C_ID].get_size();
    std::cout<<"size is "<<x<<std::endl;
}

void CPU::CAPACITY(int C_ID){
    if (C_ID < 0 || (N_cores-1) < C_ID ){
        std::cout<<"failure"<<std::endl;
        return;
    }

    int c = cores[C_ID].get_capacity();
    std::cout<<"capacity is "<<c<<std::endl;

}

void CPU::EXIT(){
    delete[] cores;
    cores = nullptr;
}