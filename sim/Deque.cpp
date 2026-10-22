#include "Deque.hpp"
#include <iostream>


Deque::Deque(int cap) {
    arr = new int[cap];
    capacity = cap;
    size = 0;
    front = 0;
    back = 0;
}

Deque::~Deque() {
    delete[] arr;
    arr = nullptr;
}

//Double the array when size reaches capacity
void Deque::resize() {
    int new_capacity = 2 * capacity;
    int *new_arr = new int[new_capacity];

    // Paste elements over 
    for (int i = 0; i < size; i++) {
        new_arr[i] = arr[(front + i) % capacity];
    }

    front = 0;
    back = size;

    delete[] arr;
    arr = new_arr;
    capacity = new_capacity;
}

//Half array when size reaches 1/4 of capacity
void Deque::shrink() {
    // Ensure the capacity doesn't go below 2
    if (capacity / 2 >= 2) {
        int new_capacity = capacity / 2;
        int *new_arr = new int[new_capacity];

        // Copy the elements to the new array
        for (int i = 0; i < size; i++) {
            new_arr[i] = arr[(front + i) % capacity];  // Handle wrap-around
        }

        front = 0;
        back = size;


        delete[] arr;
        arr = new_arr;
        capacity = new_capacity;
    }
}


bool Deque::isEmpty() const {
    return size == 0;
}

void Deque::push_b(int value) {
    arr[back] = value;
    back = (back + 1) % capacity;
    size++;

    //call resize if full
    if (size == capacity) {
        resize();  
    }
    if (size > 0 && size <= capacity / 4) {
        shrink();
    }
}

void Deque::push_f(int value) {
    //move front backward
    front = (front - 1 + capacity) % capacity;  
    arr[front] = value;
    size++;

    //call resize if full
    if (size == capacity) {
        resize(); 
    }
    if (size > 0 && size <= capacity / 4) {
        shrink();
    }
}


int Deque::pop_b() {
    if (isEmpty()) {
       return -1;
    }

    back = (back - 1 + capacity) % capacity;
    int value = arr[back];
    size--;

    // Check if we need to shrink the array
    if (size > 0 && size <= capacity / 4) {
        shrink();
    }

    return value;
}


int Deque::pop_f() {

    if (isEmpty()) {
       return -1;
    }

    int value = arr[front];
    front = (front + 1) % capacity;
    size--;

    // Check if we need to shrink the array
    if (size > 0 && size <= capacity / 4) {
        shrink();
    }

    return value;
}


int Deque::get_f() const {
    if (isEmpty()) {
       return -1;
    }
    return arr[front];
}


int Deque::get_b() const {
    if (isEmpty()) {
       return -1;

    }
    return arr[(back - 1 + capacity) % capacity];
}

int Deque::get_size() const{
    return size;
}

int Deque::get_capacity() const{
    return capacity;
}