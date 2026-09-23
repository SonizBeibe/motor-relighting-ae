#include <opencv2/opencv.hpp>
#include <iostream>
#include <chrono>

static inline float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

int main(int argc, char** argv)
{
    std::string imgPath = argc > 1 ? argv[1] : "yuka_flat.png";
    std::string cascadePath = argc > 2 ? argv[2] : "lbpcascade_animeface.xml";

    try
    {
        cv::Mat bgr = cv::imread(imgPath, cv::IMREAD_COLOR);
        if (bgr.empty())
        {
            std::cerr << "FAILED to load image: " << imgPath << std::endl;
            return 1;
        }
        std::cout << "Loaded image: " << bgr.cols << "x" << bgr.rows << std::endl;

        const int width = bgr.cols;
        const int height = bgr.rows;

        auto t0 = std::chrono::high_resolution_clock::now();

        cv::Mat luma(height, width, CV_32FC1);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                cv::Vec3b p = bgr.at<cv::Vec3b>(y, x);
                float l = 0.299f * p[2] + 0.587f * p[1] + 0.114f * p[0];
                luma.at<float>(y, x) = l / 255.0f;
            }
        std::cout << "Step 0 (luma) OK" << std::endl;

        cv::Mat blurred_luma;
        cv::GaussianBlur(luma, blurred_luma, cv::Size(0, 0), 2.0);
        std::cout << "Step 1a (blur) OK" << std::endl;

        cv::Mat ink_mask(height, width, CV_8UC1);
        cv::Mat silhouette_mask(height, width, CV_8UC1);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                float b_luma = blurred_luma.at<float>(y, x);
                ink_mask.at<uchar>(y, x) = (b_luma < 0.35f) ? 0 : 255;
                silhouette_mask.at<uchar>(y, x) = 255; // no alpha channel in this flattened test image
            }
        std::cout << "Step 1b (masks) OK" << std::endl;

        cv::Mat labels, stats, centroids;
        int num_labels = cv::connectedComponentsWithStats(ink_mask, labels, stats, centroids, 8, CV_32S);
        std::cout << "Step 2a (connectedComponents ink) OK, num_labels=" << num_labels << std::endl;

        cv::Mat dist_fine;
        cv::distanceTransform(ink_mask, dist_fine, cv::DIST_L2, 5);
        std::cout << "Step 2b (distanceTransform fine) OK" << std::endl;

        cv::Mat normalized_dist_fine = cv::Mat::zeros(height, width, CV_32FC1);
        std::vector<float> max_dist_per_label(num_labels, 0.0f);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                int label = labels.at<int>(y, x);
                float d = dist_fine.at<float>(y, x);
                if (d > max_dist_per_label[label]) max_dist_per_label[label] = d;
            }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                int label = labels.at<int>(y, x);
                float max_d = max_dist_per_label[label];
                if (max_d > 0.0f)
                    normalized_dist_fine.at<float>(y, x) = std::min(1.0f, dist_fine.at<float>(y, x) / max_d);
            }
        std::cout << "Step 2c (normalize fine) OK" << std::endl;

        cv::Mat dist_coarse;
        cv::distanceTransform(silhouette_mask, dist_coarse, cv::DIST_L2, 5);
        int num_labels_coarse = cv::connectedComponentsWithStats(silhouette_mask, labels, stats, centroids, 8, CV_32S);
        std::cout << "Step 2d (coarse dt+cc) OK, num_labels_coarse=" << num_labels_coarse << std::endl;

        cv::Mat normalized_dist_coarse = cv::Mat::zeros(height, width, CV_32FC1);
        std::vector<float> max_dist_per_label_coarse(num_labels_coarse, 0.0f);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                int label = labels.at<int>(y, x);
                float d = dist_coarse.at<float>(y, x);
                if (d > max_dist_per_label_coarse[label]) max_dist_per_label_coarse[label] = d;
            }
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                int label = labels.at<int>(y, x);
                float max_d = max_dist_per_label_coarse[label];
                if (max_d > 0.0f)
                    normalized_dist_coarse.at<float>(y, x) = std::min(1.0f, dist_coarse.at<float>(y, x) / max_d);
            }
        std::cout << "Step 2e (normalize coarse) OK" << std::endl;

        float heightExponent = 0.28f; // matches the screenshot's slider value
        cv::Mat height_fine = cv::Mat::zeros(height, width, CV_32FC1);
        cv::Mat height_coarse = cv::Mat::zeros(height, width, CV_32FC1);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
            {
                height_fine.at<float>(y, x) = std::pow(normalized_dist_fine.at<float>(y, x), heightExponent);
                height_coarse.at<float>(y, x) = std::pow(normalized_dist_coarse.at<float>(y, x), heightExponent);
            }
        std::cout << "Step 3a (pow) OK" << std::endl;

        float blurRadius = 18.0f; // matches screenshot
        cv::Mat blurred_height_fine, blurred_height_coarse;
        cv::GaussianBlur(height_fine, blurred_height_fine, cv::Size(0, 0), blurRadius);
        cv::GaussianBlur(height_coarse, blurred_height_coarse, cv::Size(0, 0), blurRadius);
        std::cout << "Step 3b (height blur) OK" << std::endl;

        cv::Mat dx_fine, dy_fine, dx_coarse, dy_coarse;
        cv::Sobel(blurred_height_fine, dx_fine, CV_32F, 1, 0, 3);
        cv::Sobel(blurred_height_fine, dy_fine, CV_32F, 0, 1, 3);
        cv::Sobel(blurred_height_coarse, dx_coarse, CV_32F, 1, 0, 3);
        cv::Sobel(blurred_height_coarse, dy_coarse, CV_32F, 0, 1, 3);
        std::cout << "Step 4a (sobel) OK" << std::endl;

        cv::CascadeClassifier face_cascade;
        bool loaded = face_cascade.load(cascadePath);
        std::cout << "Cascade loaded: " << loaded << std::endl;

        std::vector<cv::Rect> faces;
        if (loaded && !face_cascade.empty())
        {
            cv::Mat luma_8u;
            luma.convertTo(luma_8u, CV_8UC1, 255.0);
            cv::equalizeHist(luma_8u, luma_8u);
            face_cascade.detectMultiScale(luma_8u, faces, 1.02, 2, 0, cv::Size(24, 24));
            std::cout << "Step 4b (face detect) OK, faces=" << faces.size() << std::endl;
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        std::cout << "TOTAL TIME: " << std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() << "ms" << std::endl;
        std::cout << "ALL STEPS COMPLETED SUCCESSFULLY" << std::endl;
    }
    catch (const cv::Exception& e)
    {
        std::cerr << "cv::Exception CAUGHT: " << e.what() << std::endl;
        return 2;
    }
    catch (const std::exception& e)
    {
        std::cerr << "std::exception CAUGHT: " << e.what() << std::endl;
        return 3;
    }

    return 0;
}
