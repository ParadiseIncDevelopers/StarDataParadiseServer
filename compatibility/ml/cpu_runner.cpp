#include "cpu_compute.hpp"

#include <exception>
#include <iostream>

int main()
{
    try
    {
        const auto request = sdp::wire::Json::parse(std::cin);
        const auto& action = request.at("action").get_ref<const std::string&>();
        if (action != "train" && action != "predict")
        {
            std::cerr << "Unknown CPU model operation.\n";
            return 1;
        }
        std::cout << sdp::mysql_model::compute(request).dump() << '\n';
    }
    catch (const std::exception&)
    {
        std::cerr << "The original CPU model engine rejected the request.\n";
        return 1;
    }
}
