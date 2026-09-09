#include <opencv2/opencv.hpp>
#include <iostream>
#include <vector>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <algorithm>

struct StagePos {
    double x;
    double y;
};

struct SpotInfo {
    int id;
    cv::Point2f cameraCenter;   // 鍏�?�枒鍦ㄧ浉鏈哄浘鍍忎腑鐨�?腑蹇�?
    cv::Point2f sampleOffset;   // 璇ュ厜鏂戠浉瀵逛簬鎵弿涓績鐨勬牱鍝佸潗鏍囧亸绉�?
    float radius;
    float gain;
};

struct FrameData {
    cv::Mat image;
    StagePos stage;
};

static cv::Point scaledPoint(int x, int y, int width, int height) {
    return cv::Point(
        static_cast<int>(std::round(x * width / 512.0)),
        static_cast<int>(std::round(y * height / 512.0))
    );
}

static cv::Mat createGroundTruth(int width, int height) {
    cv::Mat gt = cv::Mat::zeros(height, width, CV_32FC1);

    // �?℃嫙绾崇背绾�? / 寰�?氶亾銆傚潗鏍囨寜 512 鍩哄�?缂╂斁锛屾柟渚垮垏鎹㈠埌 1024/2048銆�
    cv::line(gt, scaledPoint(60, 100, width, height), scaledPoint(450, 120, width, height), cv::Scalar(1.0f), 4);
    cv::line(gt, scaledPoint(80, 250, width, height), scaledPoint(430, 250, width, height), cv::Scalar(0.8f), 5);
    cv::line(gt, scaledPoint(120, 400, width, height), scaledPoint(400, 340, width, height), cv::Scalar(1.0f), 4);
    cv::line(gt, scaledPoint(80, 360, width, height), scaledPoint(470, 460, width, height), cv::Scalar(0.7f), 3);
    cv::line(gt, scaledPoint(40, 180, width, height), scaledPoint(460, 70, width, height), cv::Scalar(0.6f), 2);

    // 澧炲姞缁嗗皬缁撴�?瀵嗗害锛屼娇閲嶅缓鍚庣殑鎻掑�?煎拰鍘�?�櫔鏇村洶闅俱€�?
    for (int i = 0; i < 160; ++i) {
        int x = 30 + (i * 67) % 460;
        int y = 35 + (i * 97) % 440;
        int rr = 2 + (i % 4);
        float val = 0.35f + 0.55f * static_cast<float>((i * 13) % 100) / 100.0f;
        cv::circle(gt, scaledPoint(x, y, width, height), rr, cv::Scalar(val), -1);
    }

    // �?℃嫙鍦嗗�? / 鐜舰缁撴�?
    cv::circle(gt, scaledPoint(260, 260, width, height), static_cast<int>(55 * width / 512.0), cv::Scalar(0.5f), 4);
    cv::circle(gt, scaledPoint(360, 160, width, height), static_cast<int>(30 * width / 512.0), cv::Scalar(0.9f), 3);
    cv::circle(gt, scaledPoint(170, 150, width, height), static_cast<int>(38 * width / 512.0), cv::Scalar(0.65f), 3);

    // 浣庨鑳屾櫙绾圭悊锛屽鍔犲悗缁儗鏅墸闄ら�?�搴︺€�?
    for (int y = 0; y < gt.rows; ++y) {
        float* row = gt.ptr<float>(y);
        for (int x = 0; x < gt.cols; ++x) {
            float texture = 0.04f * std::sin(0.018f * x) + 0.035f * std::cos(0.014f * y);
            row[x] = std::max(0.0f, row[x] + texture);
        }
    }

    // 杞�?�井�?＄硦锛屾ā鎷熺湡瀹炶崸鍏夊垎�?�?
    cv::GaussianBlur(gt, gt, cv::Size(0, 0), 1.2);

    return gt;
}

static std::vector<SpotInfo> createSpotArray(
    int rows,
    int cols,
    int cameraWidth,
    int cameraHeight,
    float cameraSpacing,
    float sampleSpacing
) {
    std::vector<SpotInfo> spots;
    int id = 0;

    float cx = cameraWidth * 0.5f;
    float cy = cameraHeight * 0.5f;

    float sx = -(cols - 1) * cameraSpacing * 0.5f;
    float sy = -(rows - 1) * cameraSpacing * 0.5f;

    float ox = -(cols - 1) * sampleSpacing * 0.5f;
    float oy = -(rows - 1) * sampleSpacing * 0.5f;

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            SpotInfo s;
            s.id = id++;
            s.cameraCenter = cv::Point2f(
                cx + sx + c * cameraSpacing,
                cy + sy + r * cameraSpacing
            );
            s.sampleOffset = cv::Point2f(
                ox + c * sampleSpacing,
                oy + r * sampleSpacing
            );
            s.radius = 8.0f;

            // �?℃嫙澶氱劍鐐瑰厜�?轰笉鍧囧寑锛氳鍒�?�浐瀹氬樊�??�? + 缂撴參绌�?棿璋冨埗銆�
            s.gain = 0.72f
                + 0.42f * static_cast<float>((r * 3 + c * 2) % 7) / 6.0f
                + 0.06f * std::sin(0.7f * r + 0.35f * c);

            spots.push_back(s);
        }
    }

    return spots;
}

static float sampleBilinear(const cv::Mat& img, float x, float y) {
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x0 < 0 || y0 < 0 || x1 >= img.cols || y1 >= img.rows) {
        return 0.0f;
    }

    float dx = x - x0;
    float dy = y - y0;

    float v00 = img.at<float>(y0, x0);
    float v10 = img.at<float>(y0, x1);
    float v01 = img.at<float>(y1, x0);
    float v11 = img.at<float>(y1, x1);

    float v0 = v00 * (1.0f - dx) + v10 * dx;
    float v1 = v01 * (1.0f - dx) + v11 * dx;

    return v0 * (1.0f - dy) + v1 * dy;
}

static void drawGaussianSpot(
    cv::Mat& frame,
    cv::Point2f center,
    float amplitude,
    float sigma
) {
    int radius = static_cast<int>(std::ceil(3.0f * sigma));

    for (int yy = -radius; yy <= radius; ++yy) {
        for (int xx = -radius; xx <= radius; ++xx) {
            int px = static_cast<int>(std::round(center.x)) + xx;
            int py = static_cast<int>(std::round(center.y)) + yy;

            if (px < 0 || py < 0 || px >= frame.cols || py >= frame.rows) {
                continue;
            }

            float d2 = static_cast<float>(xx * xx + yy * yy);
            float val = amplitude * std::exp(-d2 / (2.0f * sigma * sigma));
            frame.at<float>(py, px) += val;
        }
    }
}

static FrameData simulateFrame(
    const cv::Mat& groundTruth,
    const std::vector<SpotInfo>& spots,
    StagePos stage,
    int cameraWidth,
    int cameraHeight,
    int frameIndex
) {
    cv::Mat frame(cameraHeight, cameraWidth, CV_32FC1, cv::Scalar(35.0f));

    // 鍔犲叆缂撴參鍙樺寲鐨�?儗鏅搴︼紝妯℃嫙鐩告満鏆�?�數娴� / 鐓ф�?�涓嶅潎銆�
    for (int y = 0; y < frame.rows; ++y) {
        float* row = frame.ptr<float>(y);
        for (int x = 0; x < frame.cols; ++x) {
            row[x] += 10.0f * static_cast<float>(x) / cameraWidth
                   + 7.0f * static_cast<float>(y) / cameraHeight
                   + 3.0f * std::sin(0.01f * x + 0.013f * y + 0.03f * frameIndex);
        }
    }

    // 浜氬儚绱犳満姊版紓绉伙紝璁╁悓涓�? stage 浣嶇疆骞朵笉瀹屽叏绛変环銆�
    float driftX = 0.35f * std::sin(0.017f * frameIndex);
    float driftY = 0.30f * std::cos(0.013f * frameIndex);

    for (const auto& spot : spots) {
        float sampleX = static_cast<float>(stage.x + spot.sampleOffset.x) + driftX;
        float sampleY = static_cast<float>(stage.y + spot.sampleOffset.y) + driftY;

        float intensity = sampleBilinear(groundTruth, sampleX, sampleY);
        float temporalGain = 1.0f + 0.08f * std::sin(0.021f * frameIndex + 0.37f * spot.id);
        float amplitude = 2600.0f * intensity * spot.gain * temporalGain;

        // 姣忎釜鍏夋枒杞�?�井鎶栧姩鍜屼笉鍚� PSF 瀹藉害锛屾彁楂樺畾浣�? / 瑙ｅ嵎绉�?�搴︺€�?
        cv::Point2f jitteredCenter(
            spot.cameraCenter.x + 0.28f * std::sin(0.19f * frameIndex + 0.11f * spot.id),
            spot.cameraCenter.y + 0.25f * std::cos(0.17f * frameIndex + 0.07f * spot.id)
        );
        float sigma = 2.4f + 0.35f * std::sin(0.5f * spot.id);
        drawGaussianSpot(frame, jitteredCenter, amplitude, sigma);
    }

    // 鏇村己璇诲嚭鍣０锛涘抚鏁板鍚庡骞冲潎鍜岄瞾�?掗噸寤虹畻娉曡姹傛洿�?�樸�?�?
    cv::Mat noise(frame.size(), CV_32FC1);
    cv::randn(noise, 0.0, 18.0);
    frame += noise;

    // 闄愬�?
    cv::max(frame, 0.0, frame);

    return FrameData{frame, stage};
}

static double extractSpotIntensity(
    const cv::Mat& frame,
    const SpotInfo& spot,
    float background = 35.0f
) {
    int cx = static_cast<int>(std::round(spot.cameraCenter.x));
    int cy = static_cast<int>(std::round(spot.cameraCenter.y));
    int r = static_cast<int>(std::round(spot.radius));

    double sum = 0.0;
    int count = 0;

    for (int yy = -r; yy <= r; ++yy) {
        for (int xx = -r; xx <= r; ++xx) {
            if (xx * xx + yy * yy > r * r) {
                continue;
            }

            int px = cx + xx;
            int py = cy + yy;

            if (px < 0 || py < 0 || px >= frame.cols || py >= frame.rows) {
                continue;
            }

            float v = frame.at<float>(py, px) - background;
            if (v < 0.0f) {
                v = 0.0f;
            }

            sum += v;
            count++;
        }
    }

    if (count == 0) {
        return 0.0;
    }

    return sum / static_cast<double>(count);
}

static cv::Mat reconstructImage(
    const std::vector<FrameData>& frames,
    const std::vector<SpotInfo>& spots,
    int outputWidth,
    int outputHeight
) {
    cv::Mat accum(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));
    cv::Mat weight(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));

    for (const auto& frame : frames) {
        for (const auto& spot : spots) {
            double signal = extractSpotIntensity(frame.image, spot);

            float sampleX = static_cast<float>(frame.stage.x + spot.sampleOffset.x);
            float sampleY = static_cast<float>(frame.stage.y + spot.sampleOffset.y);

            int x = static_cast<int>(std::round(sampleX));
            int y = static_cast<int>(std::round(sampleY));

            if (x < 0 || y < 0 || x >= outputWidth || y >= outputHeight) {
                continue;
            }

            accum.at<float>(y, x) += static_cast<float>(signal / std::max(spot.gain, 1e-6f));
            weight.at<float>(y, x) += 1.0f;
        }
    }

    cv::Mat recon(outputHeight, outputWidth, CV_32FC1, cv::Scalar(0));

    for (int y = 0; y < outputHeight; ++y) {
        for (int x = 0; x < outputWidth; ++x) {
            float w = weight.at<float>(y, x);
            if (w > 0.0f) {
                recon.at<float>(y, x) = accum.at<float>(y, x) / w;
            }
        }
    }

    // 琛ョ偣鍜屽钩婊戙�?傜湡瀹炵郴缁熼噷鍙互鎹㈡垚鏇村鏉傜殑鎻掑�?笺€�?
    cv::GaussianBlur(recon, recon, cv::Size(0, 0), 0.8);

    return recon;
}

static cv::Mat normalizeTo16U(const cv::Mat& src32f) {
    cv::Mat norm32f;
    cv::normalize(src32f, norm32f, 0, 65535, cv::NORM_MINMAX);

    cv::Mat dst16u;
    norm32f.convertTo(dst16u, CV_16UC1);

    return dst16u;
}

static void saveCsv(
    const std::string& path,
    const std::vector<FrameData>& frames,
    const std::vector<SpotInfo>& spots
) {
    std::ofstream f(path);
    f << "frame_id,stage_x,stage_y\n";
    for (size_t i = 0; i < frames.size(); ++i) {
        f << i << "," << frames[i].stage.x << "," << frames[i].stage.y << "\n";
    }
    f.close();

    std::ofstream s("spots.csv");
    s << "spot_id,camera_x,camera_y,sample_offset_x,sample_offset_y,radius,gain\n";
    for (const auto& spot : spots) {
        s << spot.id << ","
          << spot.cameraCenter.x << ","
          << spot.cameraCenter.y << ","
          << spot.sampleOffset.x << ","
          << spot.sampleOffset.y << ","
          << spot.radius << ","
          << spot.gain << "\n";
    }
    s.close();
}

int main() {
    const int sampleWidth = 1024;
    const int sampleHeight = 1024;
    const int cameraWidth = 1024;
    const int cameraHeight = 1024;

    const int scanStartX = 220;
    const int scanEndX = 460;
    const int scanStartY = 220;
    const int scanEndY = 460;
    const int scanStep = 2;

    std::filesystem::create_directories("frames");

    cv::Mat groundTruth = createGroundTruth(sampleWidth, sampleHeight);
    cv::imwrite("ground_truth.tif", normalizeTo16U(groundTruth));

    std::vector<SpotInfo> spots = createSpotArray(
        11,
        14,
        cameraWidth,
        cameraHeight,
        58.0f,
        42.0f
    );

    // ��ѡ������ɨ��λ��
    std::ofstream scanCsv("scan_positions.csv");
    scanCsv << "frame_id,stage_x,stage_y\n";

    // ��ѡ�������߲���
    std::ofstream spotCsv("spots.csv");
    spotCsv << "spot_id,camera_x,camera_y,sample_offset_x,sample_offset_y,radius,gain\n";
    for (const auto& spot : spots) {
        spotCsv << spot.id << ","
                << spot.cameraCenter.x << ","
                << spot.cameraCenter.y << ","
                << spot.sampleOffset.x << ","
                << spot.sampleOffset.y << ","
                << spot.radius << ","
                << spot.gain << "\n";
    }
    spotCsv.close();

    int frameId = 0;

    for (int sy = scanStartY; sy < scanEndY; sy += scanStep) {
        for (int sx = scanStartX; sx < scanEndX; sx += scanStep) {
            StagePos pos;
            pos.x = sx;
            pos.y = sy;

            FrameData fd = simulateFrame(
                groundTruth,
                spots,
                pos,
                cameraWidth,
                cameraHeight,
                frameId
            );

            cv::Mat out16 = normalizeTo16U(fd.image);

            char filename[256];
            std::snprintf(filename, sizeof(filename), "frames/frame_%05d.tif", frameId);
            cv::imwrite(filename, out16);

            scanCsv << frameId << "," << pos.x << "," << pos.y << "\n";

            if (frameId % 100 == 0) {
                std::cout << "Saved frame " << frameId << std::endl;
            }

            frameId++;

            // �����ͷŵ�ǰ֡�����ͼ�������ڴ����
            fd.image.release();
            out16.release();
        }
    }

    scanCsv.close();

    std::cout << "Simulation finished.\n";
    std::cout << "Frames generated: " << frameId << "\n";
    std::cout << "Spots per frame: " << spots.size() << "\n";
    std::cout << "Generated files:\n";
    std::cout << "  ground_truth.tif\n";
    std::cout << "  scan_positions.csv\n";
    std::cout << "  spots.csv\n";
    std::cout << "  frames/frame_XXXXX.tif\n";

    return 0;
}