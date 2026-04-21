// Implementation for Problem 120 - TimingWheel
// This header defines TaskNode, TimingWheel, and Timer with a three-level timing wheel.

#pragma once

#include "Task.hpp"
#include <vector>

class TimingWheel; // forward declaration

class TaskNode {
    friend class TimingWheel;
    friend class Timer;
public:
    TaskNode() : task(nullptr), next(nullptr), prev(nullptr), wheel(nullptr), slot_index(-1), rounds(0), rem(0), active(false) {}

private:
    Task* task;             // associated task
    TaskNode* next;         // for doubly linked bucket list
    TaskNode* prev;
    TimingWheel* wheel;     // which wheel currently holds this node (nullptr if none)
    int slot_index;         // slot index in the current wheel
    unsigned long long rounds; // number of full rotations remaining before processing this slot
    unsigned int rem;       // remaining lower-level delay (in base ticks of lower wheel)
    bool active;            // whether the node is currently scheduled
};

class TimingWheel {
    friend class Timer;
public:
    TimingWheel(size_t size, size_t interval)
        : size(size), interval(interval), current_slot(0) {
        buckets = new TaskNode*[size];
        for (size_t i = 0; i < size; ++i) buckets[i] = nullptr;
    }

    ~TimingWheel() {
        delete[] buckets;
        buckets = nullptr;
    }

private:
    const size_t size;      // number of slots
    const size_t interval;  // base ticks per slot step for this wheel
    size_t current_slot = 0;
    TaskNode** buckets;     // array of bucket heads
};

class Timer {
public:
    Timer()
        : sec_wheel(60, 1),
          min_wheel(60, 60),
          hour_wheel(24, 3600) {}

    ~Timer() {}

    // Add a task and return a handle (TaskNode*) to manage/cancel it later.
    TaskNode* addTask(Task* task) {
        if (!task) return nullptr;
        TaskNode* node = new TaskNode();
        node->task = task;
        insertDelay(node, getFirstInterval(task));
        return node;
    }

    // Cancel a scheduled task
    void cancelTask(TaskNode *p) {
        if (!p) return;
        if (p->active) unlinkNode(p);
        delete p;
    }

    // Advance by one base tick and return due tasks (order not specified)
    std::vector<Task*> tick() {
        // advance time counter for tasks if available
        incTaskTime();

        // advance seconds wheel
        sec_wheel.current_slot = (sec_wheel.current_slot + 1) % sec_wheel.size;

        // cascading on boundaries before executing the current second slot
        if (sec_wheel.current_slot == 0) {
            // advance minutes
            min_wheel.current_slot = (min_wheel.current_slot + 1) % min_wheel.size;

            if (min_wheel.current_slot == 0) {
                // advance hours
                hour_wheel.current_slot = (hour_wheel.current_slot + 1) % hour_wheel.size;
                // cascade from hours to minutes
                cascade(hour_wheel, min_wheel);
            }
            // cascade from minutes to seconds
            cascade(min_wheel, sec_wheel);
        }

        // Now execute tasks in the current seconds slot
        std::vector<Task*> due;
        const int slot = static_cast<int>(sec_wheel.current_slot);
        TaskNode* head = sec_wheel.buckets[slot];
        sec_wheel.buckets[slot] = nullptr;
        while (head) {
            TaskNode* node = head;
            head = head->next;

            node->next = node->prev = nullptr; // detach from extracted list

            if (node->rounds > 0) {
                // not yet, decrease rounds and reinsert into same slot
                node->rounds -= 1;
                linkAtHead(&sec_wheel, slot, node);
                continue;
            }

            // due now
            node->active = false; // temporarily not scheduled
            node->wheel = nullptr;
            node->slot_index = -1;
            due.push_back(node->task);

            // reschedule based on period
            unsigned long long period = getPeriod(node->task);
            if (period > 0) {
                insertDelay(node, period);
            } else {
                // period 0 -> do not reschedule; free the node
                delete node;
            }
        }
        return due;
    }

private:
    TimingWheel sec_wheel;  // seconds wheel: 60 slots of 1s each
    TimingWheel min_wheel;  // minutes wheel: 60 slots, 60s per slot
    TimingWheel hour_wheel; // hours wheel: 24 slots, 3600s per slot

    // Utility to safely call Task::incTime() if available
    inline void incTaskTime();
    inline unsigned long long getFirstInterval(Task* t) const;
    inline unsigned long long getPeriod(Task* t) const;

    // Insert a node after a delay (in base ticks)
    void insertDelay(TaskNode* node, unsigned long long delay) {
        // normalize negative/zero delays
        unsigned long long d = delay;
        // Choose appropriate wheel
        TimingWheel* wheel;
        unsigned long long interval;
        size_t wheel_size;
        if (d < sec_wheel.size * sec_wheel.interval) {
            wheel = &sec_wheel;
            interval = sec_wheel.interval; // 1
            wheel_size = sec_wheel.size;
        } else if (d < min_wheel.size * min_wheel.interval) {
            wheel = &min_wheel;
            interval = min_wheel.interval; // 60
            wheel_size = min_wheel.size;
        } else {
            wheel = &hour_wheel;
            interval = hour_wheel.interval; // 3600
            wheel_size = hour_wheel.size;
        }

        unsigned long long units = d / interval; // how many slots to move on chosen wheel
        unsigned long long remainder = d % interval; // remainder for lower-level wheels

        unsigned long long rounds = 0;
        unsigned long long adv = units;
        if (adv >= wheel_size) {
            rounds = adv / wheel_size;
            adv = adv % wheel_size;
        }
        size_t slot = static_cast<size_t>((wheel->current_slot + adv) % wheel_size);

        // unlink from previous position if active
        if (node->active) unlinkNode(node);

        node->wheel = wheel;
        node->slot_index = static_cast<int>(slot);
        node->rounds = rounds;
        node->rem = static_cast<unsigned int>(remainder);
        node->active = true;
        linkAtHead(wheel, static_cast<int>(slot), node);
    }

    // Cascade from higher wheel to lower wheel when higher wheel hits current slot
    void cascade(TimingWheel& from, TimingWheel& to) {
        const int slot = static_cast<int>(from.current_slot);
        TaskNode* head = from.buckets[slot];
        from.buckets[slot] = nullptr;
        while (head) {
            TaskNode* node = head;
            head = head->next;
            node->next = node->prev = nullptr;
            if (node->rounds > 0) {
                node->rounds -= 1;
                // put back into the same slot
                linkAtHead(&from, slot, node);
                continue;
            }

            // move to lower wheel with delay equal to remainder
            unsigned long long delay = node->rem;
            node->active = false;
            node->wheel = nullptr;
            node->slot_index = -1;
            insertDelay(node, delay);
        }
    }

    // Link/unlink helpers for bucket management
    void linkAtHead(TimingWheel* wheel, int slot, TaskNode* node) {
        node->prev = nullptr;
        node->next = wheel->buckets[slot];
        if (wheel->buckets[slot]) wheel->buckets[slot]->prev = node;
        wheel->buckets[slot] = node;
        node->wheel = wheel;
        node->slot_index = slot;
        node->active = true;
    }

    void unlinkNode(TaskNode* node) {
        if (!node || !node->active || !node->wheel) return;
        TimingWheel* wheel = node->wheel;
        int slot = node->slot_index;
        if (node->prev) node->prev->next = node->next;
        else wheel->buckets[slot] = node->next;
        if (node->next) node->next->prev = node->prev;
        node->next = node->prev = nullptr;
        node->wheel = nullptr;
        node->slot_index = -1;
        node->active = false;
    }
};

// Minimal inline adapters for Task methods
inline void Timer::incTaskTime() {
    Task::incTime();
}

inline unsigned long long Timer::getFirstInterval(Task* t) const {
    // Task::getFirstInterval should exist
    return static_cast<unsigned long long>(t->getFirstInterval());
}

inline unsigned long long Timer::getPeriod(Task* t) const {
    return static_cast<unsigned long long>(t->getPeriod());
}
