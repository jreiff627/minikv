
## Stage 1 - Sequential TCP Server



## Stage 3 - Reader Writer locking
Below is a sample script to really test the concurrency with different readers and writers at a fast pace
    for i in {1..50}; do
        (
            echo "PUT key$((i % 10)) val$i"
            echo "GET key$((i % 10))"
            echo "QUIT"
        ) | nc localhost 9000 &
    done
    wait