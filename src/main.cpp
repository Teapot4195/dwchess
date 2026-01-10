#include <iostream>
#include <thread>

#include <chess.h>
#include <condition_variable>
#include <mutex>
#include <random>
#include <ranges>

struct UCIOption {
    enum TYPE {
        CHECK,
        SPIN,
        COMBO,
        BUTTON,
        STRING,
    } type;

    using VALUE = std::variant<bool, int, std::string, std::monostate>;

    VALUE def;
    VALUE min;
    VALUE max;
    std::vector<VALUE> options;

    [[nodiscard]] std::string type_string() const {
        switch (type) {
            case CHECK:
                return "check";
            case SPIN:
                return "spin";
            case COMBO:
                return "combo";
            case BUTTON:
                return "button";
            case STRING:
                return "string";
        }
        throw std::runtime_error("unknown type.");
    }

    [[nodiscard]] std::string get_value_data() const {
        return "";
        // TODO:!
        // std::string result;
        // if (def.index() != 3) { // MONOSTATE (no value)
        //     result += "default ";
        //
        // }
        // switch (type) {
        //     case CHECK:
        //         break;
        //     case SPIN:
        //         break;
        //     case COMBO:
        //         break;
        //     case BUTTON:
        //         break;
        //     case STRING:
        //         break;
        // }
    }
};

struct GoFlags {
    bool infinite = true;
    std::size_t nodes = std::numeric_limits<std::size_t>::max();
    std::size_t wtime = std::numeric_limits<std::size_t>::max();
    std::size_t btime = std::numeric_limits<std::size_t>::max();
    std::size_t winc = std::numeric_limits<std::size_t>::max();
    std::size_t binc = std::numeric_limits<std::size_t>::max();
};

class UCIEngine {
    bool uci = false;
    bool debug = false;

    bool should_stop = false;
    bool searching = false;

    chess::Board gameBoard;

    std::string engineName;
    std::string engineAuthor;
    std::unordered_map<std::string, UCIOption> options;

    std::stop_source stop_;

    std::mutex lock_{};

    chess::Move curmove = chess::Move::NO_MOVE;
    int curmoven = 1;
    std::size_t hashfull = 0;
    std::size_t nps = 0;
    std::size_t ncount = 0;
    std::size_t tbhits = 0;
    int cpuload = 0;
    std::string enginestr;
    std::vector<chess::Move> curLine;

    std::condition_variable worker_to_main;

    GoFlags go_flags_;

    /*==== UCI Command Processing Region ====*/

    void cmdUCI() {
        uci = true;

        sendEngineVersion();
        sendEngineAuthor();
        sendEngineOptions();

        sendUCIOK();
    }

    void cmdDebug(const std::string &rem) {
        if (rem == "on")
            debug = true;
        else if (rem == "off")
            debug = false;
        else
            std::cerr << "Unknown debug flag: " << rem << std::endl;
    }

    void cmdIsReady() {
        sendReadyOK();
    }

    void cmdUCINewGame() {
        curmove = chess::Move::NO_MOVE;
        curmoven = 1;
        hashfull = 0;
        nps = 0;
        ncount = 0;
        tbhits = 0;
        cpuload = 0;
        enginestr = "";
        curLine.clear();

        gameBoard.setFen(chess::constants::STARTPOS);
    }

    void cmdPosition(std::string rem) {
        if (rem.starts_with("fen ")) {
            cmdUCINewGame();
            auto fpos = 4;
            auto epos = rem.find(" moves ");
            gameBoard.setFen(rem.substr(fpos, epos == std::string::npos ? epos : epos - fpos));
            rem = epos == std::string::npos ? "" : rem.substr(epos);
        } else if (rem.starts_with("startpos"))
            cmdUCINewGame();
        auto pos = rem.find("moves ");
        if (pos != std::string::npos) {
            std::string moves = rem.substr(pos + 6);
            while (!moves.empty()) {
                auto next = moves.find_first_of(' ');
                std::string move = moves.substr(0, next);
                if (next == std::string::npos)
                    moves = "";
                else
                    moves = moves.substr(next + 1);

                auto m = chess::uci::uciToMove(gameBoard, move);
                if (m == chess::Move::NO_MOVE) {
                    std::cerr << "unknown move: " << move << std::endl;
                    return;
                }
                // std::cerr << "playing: " << chess::uci::moveToUci(m) << std::endl;
                gameBoard.makeMove(m);
            }
        }
    }

    void cmdGo(std::string rem) {
        go_flags_ = {};
        while (!rem.empty()) {
            if (rem.starts_with("infinite")) {
                rem = rem.substr(8);
                go_flags_.infinite = true;
            } else if (rem.starts_with("nodes ")) {
                rem = rem.substr(6);
                auto pos = rem.find(' ');
                go_flags_.nodes = std::stoull(rem.substr(0, pos));
                rem = pos == std::string::npos ? "" : rem.substr(pos + 1);
            } else if (rem.starts_with("wtime ")) {
                rem = rem.substr(6);
                auto pos = rem.find(' ');
                go_flags_.wtime = std::stoull(rem.substr(0, pos));
                rem = pos == std::string::npos ? "" : rem.substr(pos + 1);
            } else if (rem.starts_with("btime ")) {
                rem = rem.substr(6);
                auto pos = rem.find(' ');
                go_flags_.btime = std::stoull(rem.substr(0, pos));
                rem = pos == std::string::npos ? "" : rem.substr(pos + 1);
            } else if (rem.starts_with("winc ")) {
                rem = rem.substr(5);
                auto pos = rem.find(' ');
                go_flags_.winc = std::stoull(rem.substr(0, pos));
                rem = pos == std::string::npos ? "" : rem.substr(pos + 1);
            } else if (rem.starts_with("binc ")) {
                rem = rem.substr(5);
                auto pos = rem.find(' ');
                go_flags_.binc = std::stoull(rem.substr(0, pos));
                rem = pos == std::string::npos ? "" : rem.substr(pos + 1);
            } else {
                std::cerr << "Parsed as much of go command as possible, have remaining: " << rem << std::endl;
                break;
            }
        }

        searching = true;
        worker_to_main.notify_all();
    }

    void cmdStop() {
        should_stop = true;
    }

    /*==== UCI Responses Region ====*/

    void sendEngineVersion() {
        std::lock_guard lk_(lock_);
        std::cout << "id name " << engineName << std::endl;
    }

    void sendEngineAuthor() {
        std::lock_guard lk_(lock_);
        std::cout << "id author " << engineAuthor << std::endl;
    }

    void sendEngineOptions() {
        std::lock_guard lk_(lock_);

        // NOTE: report bogus Threads and Hash values for openbench to work correctly :xdd:
        std::cout << "option name Threads type spin default 1 min 1 max 1" << std::endl;
        std::cout << "option name Hash type spin default 1 min 1 max 1" << std::endl;

        for (const auto& [opt, data] : options) {
            // std::cout << "option name " << opt << " type " << data.type_string();
        }
    }

    void sendUCIOK() {
        std::lock_guard lk_(lock_);
        std::cout << "uciok" << std::endl;
    }

    void sendReadyOK() {
        std::lock_guard lk_(lock_);
        std::cout << "readyok" << std::endl;
    }

    void sendDepth(int plies) {
        std::lock_guard lk_(lock_);
        std::cout << std::format("info depth {}", plies) << std::endl;
    }

    void sendSelDepth(int plies, int selplies) {
        std::lock_guard lk_(lock_);
        std::cout << std::format("info depth {}, seldepth {}", plies, selplies) << std::endl;
    }

    void sendStatus(std::size_t ms, const std::vector<std::vector<chess::Move>>& moves) {
        std::lock_guard lk_(lock_);
        std::string result = std::format("info time {} multipv {} ", ms, moves.size());

        for (const auto& line : moves) {
            result += "pv ";
            for (const auto& move : line) {
                result += chess::uci::moveToUci(move);
                result += ' ';
            }
        }

        std::cout << result << std::endl;
    }

    void sendScore(int val, bool ismate = false, bool lower = false, bool upper = false) {
        std::lock_guard lk_(lock_);
        if (ismate)
            std::cout << std::format("info score mate {}", val) << std::endl;
        else if (lower)
            std::cout << std::format("info score cp {} lowerbound", val) << std::endl;
        else if (upper)
            std::cout << std::format("info score cp {} upperbound", val) << std::endl;
        else
            std::cout << std::format("info score cp {}", val) << std::endl;
    }

    void sendEngineUpdate() {
        std::lock_guard lk_(lock_);
        std::string result = "info ";

        if (curmove != chess::Move::NO_MOVE)
            result += std::format("currmove {} currmovenumber {} ", chess::uci::moveToUci(curmove), curmoven);

        result += std::format("hashfull {} ", hashfull);

        result += std::format("nps {} ", nps);

        if (ncount > 0)
            result += std::format("nodes {} ", ncount);

        if (tbhits > 0)
            result += std::format("tbhits {} ", tbhits);

        if (cpuload > 0)
            result += std::format("cpuload {} ", cpuload);

        if (!enginestr.empty())
            result += std::format("string {} ", enginestr);

        if (!curLine.empty()) {
            result += "currline 0 ";
            for (const auto& move : curLine) {
                result += chess::uci::moveToUci(move);
                result += ' ';
            }
        }

        if (result.ends_with(' '))
            result.pop_back();

        std::cout << result << std::endl;
        std::cerr << result << std::endl;
    }

    void sendResult(chess::Move best, chess::Move ponder = chess::Move::NO_MOVE) {
        std::lock_guard lk_(lock_);
        std::cout << std::format("bestmove {}", chess::uci::moveToUci(best));
        if (ponder != chess::Move::NO_MOVE)
            std::cout << std::format(" ponder {}", chess::uci::moveToUci(ponder));
        std::cout << std::endl;
    }

public:
    std::stop_token should_stop_;

    UCIEngine(const std::string &name, const std::string &author, const std::unordered_map<std::string, UCIOption> &options)
        : engineName(name), engineAuthor(author), options(options) {
        should_stop_ = stop_.get_token();
    }

    virtual ~UCIEngine() = default;

    void worker() {
        while (true) {
            std::string i;
            std::getline(std::cin, i);

            // std::cerr << "received: " << i << std::endl;

            auto pos = i.find(' ');
            std::string cmd = i.substr(0, pos);
            std::string rem = pos == std::string::npos ? "" : i.substr(pos + 1);

            if (cmd == "uci")
                cmdUCI();
            else if (cmd == "debug")
                cmdDebug(rem);
            else if (cmd == "isready")
                cmdIsReady();
            else if (cmd == "ucinewgame")
                cmdUCINewGame();
            else if (cmd == "position")
                cmdPosition(rem);
            else if (cmd == "go")
                cmdGo(rem);
            else if (cmd == "stop")
                cmdStop();
            else if (cmd == "setoption")
                ;
            else if (cmd == "quit") {
                (void)stop_.request_stop();
                return;
            } else {
                std::cerr << "unknown command: " << cmd << " " << rem << std::endl;
            }
        }
    }

    [[nodiscard]] bool isUCI() const {
        return uci;
    }

    virtual void run() {
        using namespace std::chrono_literals;

        std::random_device rd;
        std::mt19937 gen(rd());

        std::mutex mine;
        while (!should_stop_.stop_requested()) {
            std::unique_lock lk_(mine);
            worker_to_main.wait(lk_, [this]{return searching;});

            std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

            chess::Movelist ml;

            chess::movegen::legalmoves(ml, gameBoard);

            auto dist = std::uniform_int_distribution<std::size_t>(0, ml.size() - 1);
            auto pv = ml[dist(gen)];

            int it = 0;
            while (!should_stop &&                                                  // asked to stop by GUI
                   go_flags_.nodes == std::numeric_limits<std::size_t>::max() &&    // should stop early (RNG engine searches exactly 0 nodes :xdd:)
                   false
            ) {
                std::this_thread::sleep_for(50ms);
                if (1s < std::chrono::steady_clock::now() - start_time)
                    sendStatus(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count(), {{pv}});
                it++;
            }

            sendScore(0);
            sendDepth(0);
            sendResult(pv);

            searching = false;
            should_stop = false;
        }
    }

    void updatesWorker() {
        using namespace std::chrono_literals;
        while (!should_stop_.stop_requested()) {
            std::this_thread::sleep_for(1s);

            if (searching) {
                sendEngineUpdate();
            }
        }
    }
};

int main(int argc, char** argv) {
    if (argc > 1) {
        std::string a2 = argv[1];
        // TODO actually implement bench
        if (a2 == "bench") {
            std::cout << "1 nps" << std::endl;
            std::cout << "1 nodes" << std::endl;
            return 0;
        }
    }

    UCIEngine engine("dwchess v0.0.1", "teapot and co.", {});
    std::jthread worker([&engine]{engine.worker();});
    std::jthread updatesWorker([&engine] {engine.updatesWorker();});

    engine.run();

    // while (board.isGameOver().second == chess::GameResult::NONE) {
    //     std::cout << std::format("it is {} to move, board state:", board.sideToMove() == chess::Color::WHITE ? "white" : "black") << std::endl;
    //     std::cout << "  A B C D E F G H\n";
    //     for (auto rank : {chess::Rank::RANK_1, chess::Rank::RANK_2, chess::Rank::RANK_3, chess::Rank::RANK_4,
    //                                   chess::Rank::RANK_5, chess::Rank::RANK_6, chess::Rank::RANK_7, chess::Rank::RANK_8}) {
    //         std::cout << static_cast<int>(chess::Rank(rank)) << " ";
    //         for (auto file : {chess::File::FILE_A, chess::File::FILE_B, chess::File::FILE_C, chess::File::FILE_D,
    //                                       chess::File::FILE_E, chess::File::FILE_F, chess::File::FILE_G, chess::File::FILE_H}) {
    //             auto p = board.at(chess::Square{file, rank});
    //             char c;
    //             switch (p.type().internal()) {
    //                 case chess::PieceType::PAWN:
    //                     c = 'p';
    //                     break;
    //                 case chess::PieceType::KING:
    //                     c = 'k';
    //                     break;
    //                 case chess::PieceType::KNIGHT:
    //                     c = 'n';
    //                     break;
    //                 case chess::PieceType::BISHOP:
    //                     c = 'b';
    //                     break;
    //                 case chess::PieceType::ROOK:
    //                     c = 'r';
    //                     break;
    //                 case chess::PieceType::QUEEN:
    //                     c = 'q';
    //                     break;
    //                 case chess::PieceType::NONE:
    //                     c = ' ';
    //                     break;
    //                 default:
    //                     c = '?';
    //                     break;
    //             }
    //
    //             if (p.color() == chess::Color::WHITE)
    //                 c = static_cast<char>(std::toupper(c));
    //
    //             std::cout << c << ' ';
    //         }
    //
    //         std::cout << "\n";
    //     }
    //
    //     std::string move;
    //     std::getline(std::cin, move);
    //
    //     chess::Move userMove{};
    //     try {
    //         userMove = chess::uci::parseSan(board, move);
    //     } catch (const chess::uci::SanParseError& e) {
    //         continue;
    //     }
    //
    //     board.makeMove(userMove);
    // }

    return 0;
}