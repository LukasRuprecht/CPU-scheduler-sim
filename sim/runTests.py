import subprocess
import multiprocessing
import json
import os
import random
import argparse

# Configuration
class Config:
    EXECUTABLE = './a.out'
    TESTS_FILEPATH = "fuzz/"
    MAX_LENGTH = 30
    NUM_ITERATIONS = len([f for f in os.listdir(TESTS_FILEPATH) if f.endswith(".in")])
    NUM_WORKERS = 12
    OUTPUT_DIR = "output"
    FLOAT_TOLERANCE = 1e-3
    VALGRIND_CMD = "valgrind"
    VALGRIND_ARGS = ["--leak-check=full", "--show-leak-kinds=all", "--errors-for-leak-kinds=all", "--error-exitcode=1"]

# Utility functions
def run_code(input_string, use_valgrind=False):
    if use_valgrind:
        cmd = [Config.VALGRIND_CMD] + Config.VALGRIND_ARGS + [Config.EXECUTABLE]
        process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        stdout, stderr = process.communicate(input=input_string.encode())
        return {
            'stdout': stdout.decode(),
            'stderr': stderr.decode(),
            'returncode': process.returncode
        }
    else:
        process = subprocess.run([Config.EXECUTABLE], input=input_string.encode(), capture_output=True)
        return {
            'stdout': process.stdout.decode(),
            'stderr': process.stderr.decode(),
            'returncode': process.returncode
        }

def compare_output(correct_solution, current_solution, input_string):
    if correct_solution != current_solution:
        return False
    return True

# Worker function
def worker(id, valgrind_rate):
    errors = []
    correct = 0
    total = 0

    for ci in range(id, Config.NUM_ITERATIONS, Config.NUM_WORKERS):
        i = str(ci + 1).zfill(6)
        
        with open(f"{Config.TESTS_FILEPATH}test{i}.in", "r") as f:
            input_string = f.read()

        run_valgrind = random.random() < valgrind_rate if valgrind_rate < 1.0 else True
        result = run_code(input_string, use_valgrind=run_valgrind)
        current_solution = result['stdout']
        
        with open(f"{Config.TESTS_FILEPATH}test{i}.out", "r") as f:
            correct_solution = f.read()

        total += 1
        output_correct = compare_output(correct_solution, current_solution, input_string)
        memory_safe = result['returncode'] == 0 if run_valgrind else True

        if output_correct and memory_safe:
            correct += 1
        else:
            errors.append({
                "input": input_string,
                "correct": correct_solution,
                "current": current_solution,
                "id": i,
                "output_correct": output_correct,
                "memory_safe": memory_safe,
                "valgrind_output": result['stderr'] if run_valgrind else "Valgrind not run"
            })

        if (ci - id) % (10 * Config.NUM_WORKERS) == 0:
            print(f"Worker {id}: Processed {ci - id + 1} tests")

    with open(f"{Config.OUTPUT_DIR}/workerOutput{id}.output", "w") as f:
        json.dump({"total": total, "correct": correct, "errors": errors}, f)

# Main function
def main(valgrind_rate):
    subprocess.run(["mkdir", "-p", Config.OUTPUT_DIR], check=True)

    multiprocessing.set_start_method('spawn')

    with multiprocessing.Pool(processes=Config.NUM_WORKERS) as pool:
        pool.starmap(worker, [(i, valgrind_rate) for i in range(Config.NUM_WORKERS)])

    total = 0
    correct = 0
    errors = []

    for worker_id in range(Config.NUM_WORKERS):
        with open(f"{Config.OUTPUT_DIR}/workerOutput{worker_id}.output", "r") as f:
            status = json.load(f)
        total += status["total"]
        correct += status["correct"]
        errors += status["errors"]

    for error in errors:
        print("\nID\n")
        print(error["id"])
        print("\nINPUT\n")
        print(error["input"])
        print("\nTEST OUTPUT\n")
        print(error["correct"])
        print("\nYOUR OUTPUT\n")
        print(error["current"])
        print("\nOUTPUT CORRECT\n")
        print(error["output_correct"])
        print("\nMEMORY SAFE\n")
        print(error["memory_safe"])
        if not error["memory_safe"]:
            print("\nVALGRIND OUTPUT\n")
            print(error["valgrind_output"])

    print(f"Total Tests: {total}")
    print(f"Correct Tests: {correct}")
    print(f"Valgrind Rate: {valgrind_rate:.2%}")

    with open("workerOutput.json", "w") as f:
        json.dump({"total": total, "correct": correct, "errors": errors}, f)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Run tests with optional Valgrind memory checking")
    parser.add_argument("--valgrind", nargs='?', const=1.0, type=float, 
                        help="Run Valgrind on tests. If no value is provided, run on all tests. "
                             "If a float between 0 and 1 is provided, run on that fraction of tests.")
    args = parser.parse_args()

    valgrind_rate = args.valgrind if args.valgrind is not None else 0.0
    if valgrind_rate < 0 or valgrind_rate > 1:
        parser.error("Valgrind rate must be between 0 and 1")

    main(valgrind_rate)
