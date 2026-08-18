#include "yolov11_thread_pool.h"
#include "draw/cv_draw.h"

// Constructor
Yolov11ThreadPool::Yolov11ThreadPool() { stop = false; }

// Destructor
Yolov11ThreadPool::~Yolov11ThreadPool()
{
    // stop all threads
    stop = true;
    cv_task.notify_all();
    for (auto &thread : threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

// Initialize: load model, create threads. Parameters: model path, number of threads
nn_error_e Yolov11ThreadPool::setUp(std::string &model_path, int num_threads)
{
    // Create model instances for each thread
    for (size_t i = 0; i < num_threads; ++i)
    {
        std::shared_ptr<Yolov11Custom> yolov11 = std::make_shared<Yolov11Custom>();
        yolov11->LoadModel(model_path.c_str());
        yolov11_instances.push_back(yolov11);
    }
    // Create threads
    for (size_t i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(&Yolov11ThreadPool::worker, this, i);
    }
    return NN_SUCCESS;
}

// Thread function. Parameter: thread id
void Yolov11ThreadPool::worker(int id)
{
    while (!stop)
    {
        std::pair<int, cv::Mat> task;
        std::shared_ptr<Yolov11Custom> instance = yolov11_instances[id]; // Get model instance
        {
            // Get task
            std::unique_lock<std::mutex> lock(mtx1);
            cv_task.wait(lock, [&]
                         { return !tasks.empty() || stop; });

            if (stop)
            {
                return;
            }

            task = tasks.front();
            tasks.pop();
        }
        // Run model
        std::vector<Detection> detections;
        instance->Run(task.second, detections);

        {
            // Save results
            std::lock_guard<std::mutex> lock(mtx2);
            results.insert({task.first, detections});
            DrawDetections(task.second, detections);
            img_results.insert({task.first, task.second});
        }
    }
}

// Submit task. Parameters: image, id (frame number)
nn_error_e Yolov11ThreadPool::submitTask(const cv::Mat &img, int id)
{
    // If the number of tasks in the queue is more than 10, wait to avoid excessive memory usage
    while (tasks.size() > 10)
    {
        // sleep 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        // Save task
        std::lock_guard<std::mutex> lock(mtx1);
        tasks.push({id, img});
    }
    cv_task.notify_one();
    return NN_SUCCESS;
}

// Get result. Parameters: detection boxes, id (frame number)
nn_error_e Yolov11ThreadPool::getTargetResult(std::vector<Detection> &objects, int id)
{
    // If no result, wait
    while (results.find(id) == results.end())
    {
        // sleep 1ms
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::lock_guard<std::mutex> lock(mtx2);
    objects = results[id];
    // remove from map
    results.erase(id);
    img_results.erase(id);

    return NN_SUCCESS;
}

// Get result (image). Parameters: image, id (frame number)
nn_error_e Yolov11ThreadPool::getTargetImgResult(cv::Mat &img, int id)
{
    int loop_cnt = 0;
    // If no result, wait
    while (img_results.find(id) == img_results.end())
    {
        // Wait 5ms x 1000 = 5s
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        loop_cnt++;
        if (loop_cnt > 1000)
        {
            NN_LOG_ERROR("getTargetImgResult timeout");
            return NN_TIMEOUT;
        }
    }
    std::lock_guard<std::mutex> lock(mtx2);
    img = img_results[id];
    // remove from map
    img_results.erase(id);
    results.erase(id);

    return NN_SUCCESS;
}

// Get result non-blocking. Parameters: detection boxes, id (frame number)
nn_error_e Yolov11ThreadPool::getTargetResultNonBlock(std::vector<Detection> &objects, int id)
{
    if (results.find(id) == results.end())
    {
        return NN_RESULT_NOT_READY;
    }
    std::lock_guard<std::mutex> lock(mtx2);
    objects = results[id];
    // remove from map
    results.erase(id);
    img_results.erase(id);

    return NN_SUCCESS;
}

// Stop all threads
void Yolov11ThreadPool::stopAll()
{
    stop = true;
    cv_task.notify_all();
} 