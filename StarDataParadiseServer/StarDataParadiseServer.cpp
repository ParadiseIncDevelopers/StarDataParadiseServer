#include "server.hpp"
#include "storage_admin.hpp"
#include "../storage/storage.hpp"
#include "../sql/diagnostic.hpp"

#include <iostream>

int wmain(int argc, wchar_t** argv)
{
    using namespace std::literals;
    if (argc == 1 || (argc == 2 && argv[1] == L"--help"sv))
    {
        std::cout << "Usage: StarDataParadiseServer [--help | --version]\n"
                  << "       StarDataParadiseServer --config <file> [--bootstrap-admin <username> | --initialize-storage | --check-storage | --recover-storage | --checkpoint-storage | --catalog-info | --check-sql <file>]\n";
        return 0;
    }
    if (argc == 2 && argv[1] == L"--version"sv)
    {
        std::cout << "StarDataParadiseServer " << sdp::server::product_version << '\n';
        return 0;
    }
    if ((argc != 3 && argc != 4 && argc != 5) || argv[1] != L"--config"sv
        || (argc == 4 && argv[3] != L"--initialize-storage"sv && argv[3] != L"--check-storage"sv
            && argv[3] != L"--recover-storage"sv && argv[3] != L"--checkpoint-storage"sv && argv[3] != L"--catalog-info"sv)
        || (argc == 5 && argv[3] != L"--bootstrap-admin"sv && argv[3] != L"--check-sql"sv))
    {
        std::cerr << "Invalid arguments. Use --help.\n";
        return 2;
    }

    try
    {
        const auto config = sdp::server::read_config(argv[2]);
        if (argc == 5)
        {
            if (argv[3] == L"--check-sql"sv)
            {
                sdp::server::check_sql(config, argv[4]);
                return 0;
            }
            const std::wstring_view wide(argv[4]);
            if (wide.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != wide.npos)
                throw sdp::server::StartupError("Invalid administrator username");
            std::string username;
            username.reserve(wide.size());
            for (const auto character : wide) username.push_back(static_cast<char>(character));
            sdp::server::bootstrap_admin(config, username);
            std::cout << "Administrator configured\n";
            return 0;
        }
        if (argc == 4)
        {
            if (argv[3] == L"--initialize-storage"sv) sdp::server::initialize_storage(config);
            else if (argv[3] == L"--recover-storage"sv) sdp::server::recover_storage(config);
            else if (argv[3] == L"--checkpoint-storage"sv) sdp::server::checkpoint_storage(config);
            else if (argv[3] == L"--catalog-info"sv) sdp::server::inspect_catalog(config);
            else sdp::server::inspect_storage(config);
            return 0;
        }
        return sdp::server::run(config);
    }
    catch (const sdp::server::StartupError& error)
    {
        std::cerr << error.what() << '\n';
    }
    catch (const sdp::sql::Error& error)
    {
        const auto where = error.source();
        std::cerr << "SQL " << where.line << ':' << where.column << ' ' << error.what() << '\n';
    }
    catch (const sdp::disk::Error& error)
    {
        std::cerr << "Storage error: " << error.what() << '\n';
    }
    catch (...)
    {
        std::cerr << "Server startup or runtime error\n";
    }
    return 1;
}
