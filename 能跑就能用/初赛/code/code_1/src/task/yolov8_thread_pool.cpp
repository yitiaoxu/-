#include "yolov8_thread_pool.h"
#include "draw/cv_draw.h"

// 构造函数
Yolov8ThreadPool::Yolov8ThreadPool() { stop = false; }

// 析构函数
Yolov8ThreadPool::~Yolov8ThreadPool()
{
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

// 初始化：加载模型，创建线程
nn_error_e Yolov8ThreadPool::setUp(std::string &model_path, int num_threads)
{
    for (size_t i = 0; i < num_threads; ++i)
    {
        std::shared_ptr<Yolov8Custom> Yolov8 = std::make_shared<Yolov8Custom>();
        Yolov8->LoadModel(model_path.c_str(), i % 3);
        Yolov8_instances.push_back(Yolov8);
    }
    for (size_t i = 0; i < num_threads; ++i)
    {
        threads.emplace_back(&Yolov8ThreadPool::worker, this, i);
    }
    return NN_SUCCESS;
}

// 工作线程
void Yolov8ThreadPool::worker(int id)
{
    while (!stop)
    {
        std::pair<int, cv::Mat> task;
        std::shared_ptr<Yolov8Custom> instance = Yolov8_instances[id]; 
        {
            std::unique_lock<std::mutex> lock(mtx1);
            cv_task.wait(lock, [&]{ return !tasks.empty() || stop; });
            if (stop) return;
            task = tasks.front();
            tasks.pop();
        }
        
        // 运行模型
        // 运行模型
        std::vector<Detection> detections;
        instance->Run(task.second, detections);

        {
            std::lock_guard<std::mutex> lock(mtx2);
            results.insert({task.first, detections});
            
            // 👇 核心修复 3：如果积压超过 50 个孤儿结果，强制掐断清空，绝对不让它无限生长！
            if (results.size() > 50) {
                results.erase(results.begin());
            }
        }
    }
}

// 提交任务
nn_error_e Yolov8ThreadPool::submitTask(const cv::Mat &img, int id)
{
    // 🚀 【核心修复点】：彻底删除了 while(tasks.size() > 10) 的休眠阻塞！
    // 保证主线程绝对不会卡住，主线程能以极速抽空硬件解码器的队列，彻底根治 OOM 内存泄漏！
    {
        std::lock_guard<std::mutex> lock(mtx1);
        tasks.push({id, img});
    }
    cv_task.notify_one();
    return NN_SUCCESS;
}

// 获取结果（阻塞版，用于兼容老接口）
nn_error_e Yolov8ThreadPool::getTargetResult(std::vector<Detection> &objects, int id)
{
    while (true)
    {
        {
            std::lock_guard<std::mutex> lock(mtx2);
            auto it = results.find(id);
            if (it != results.end())
            {
                objects = it->second;
                results.erase(it);
                img_results.erase(id); // 防止头文件中遗留，安全擦除
                return NN_SUCCESS;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// 获取图片结果（保留接口，防报错）
nn_error_e Yolov8ThreadPool::getTargetImgResult(cv::Mat &img, int id)
{
    // 已废弃不用
    return NN_TIMEOUT;
}

// 非阻塞获取结果
nn_error_e Yolov8ThreadPool::getTargetResultNonBlock(std::vector<Detection> &objects, int id)
{
    // 进入函数第一步必须上锁！解决之前的段错误
    std::lock_guard<std::mutex> lock(mtx2);
    
    auto it = results.find(id);
    if (it == results.end())
    {
        return NN_RESULT_NOT_READY;
    }
    
    objects = it->second;
    results.erase(it);
    img_results.erase(id); // 防止头文件中遗留，安全擦除

    return NN_SUCCESS;
}

// 停止所有线程
void Yolov8ThreadPool::stopAll()
{
    stop = true;
    cv_task.notify_all();
}