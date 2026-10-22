#ifndef DEQUE_HPP
#define DEQUE_HPP

class Deque {
    private:
        int *arr;        
        int capacity;    
        int size;        
        int front;       
        int back;        
        void resize();
        void shrink();

    public:
        
        Deque(int cap = 4);
        ~Deque();
        bool isEmpty() const;
        void push_b(int value);
        void push_f(int value);
        int pop_b();
        int pop_f();
        int get_f() const;
        int get_b() const;
        int get_size() const;
        int get_capacity() const;
};

#endif


