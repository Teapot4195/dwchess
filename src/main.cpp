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

constexpr std::pair<chess::PieceType::underlying, std::int32_t> piece_score_table[5] = {
    {chess::PieceType::PAWN, 100},
    {chess::PieceType::KNIGHT, 300},
    {chess::PieceType::BISHOP, 300},
    {chess::PieceType::ROOK, 500},
    {chess::PieceType::QUEEN, 900}
};

std::int16_t evaluate(chess::Board& board) {
    std::int16_t score = 0;

    for (auto [piece, value] : piece_score_table) {
        score += __builtin_popcountll(board.pieces(piece, chess::Color::WHITE).getBits()) * value;
        score -= __builtin_popcountll(board.pieces(piece, chess::Color::BLACK).getBits()) * value;
    }

    return score;
}

class UCIEngine;

struct SearchControl {
    UCIEngine* engine;
    std::size_t ourtime;
    std::size_t ourinc;
    std::size_t atime;

    chess::Color us, them;

    bool should_stop() const;
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

    std::chrono::steady_clock::time_point start_time;

    std::condition_variable worker_to_main;

    GoFlags go_flags_;

    friend struct SearchControl;

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
        // TODO: make this not do something this bad :xdd:
        while (searching) {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(50ms);
        }

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
        // TODO: make this not do something this bad :xdd:
        while (searching) {
            using namespace std::chrono_literals;
            std::this_thread::sleep_for(50ms);
        }

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
        // update engine nps
        auto t = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time).count();
        nps = t == 0 ? 0 : ncount / t;

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

    std::random_device rd;
    std::mt19937 rng{rd()};

    std::pair<std::int16_t, chess::Move> eval_tree(chess::Movelist& ml, chess::Board& board, std::uint8_t depth, std::int16_t alpha, std::int16_t beta, SearchControl& control, bool min) {
        // Things can overflow/underflow if we using intmin and intmax,
        constexpr std::int16_t minimum = -31000;
        constexpr std::int16_t maximum = 31000;
        auto good = min ? minimum : maximum;
        auto bad = min ? maximum : minimum;

        bool found_good = false;

        auto kingSq = gameBoard.kingSq(gameBoard.sideToMove());

        ncount += ml.size();
        for (auto& move : ml) {
            board.makeMove(move);

            if (board.inCheck()) {
                auto go = board.isGameOver();

                // fastpath for wins-losses
                switch (go.second) {
                    case chess::GameResult::WIN:
                        move.setScore(bad);
                        break;
                    case chess::GameResult::LOSE:
                        found_good = true;
                        move.setScore(good);
                        break;
                    case chess::GameResult::DRAW:
                        move.setScore(min ? 1 : -1);
                        break;
                    case chess::GameResult::NONE:
                        move.setScore(evaluate(board));
                        break;
                }
            } else
                move.setScore(evaluate(board));

            board.unmakeMove(move);
        }

        if (control.should_stop())
            goto finish;

        if (found_good)
            goto finish;

        if (min)
            std::ranges::sort(ml, std::less{}, [](const chess::Move& m) { return m.score(); });
        else
            std::ranges::sort(ml, std::greater{}, [](const chess::Move& m) { return m.score(); });

        if (min) {
            std::int16_t best = std::numeric_limits<std::int16_t>::max();

            for (auto& move : ml) {
                std::int16_t score = 0;

                // disfavor making king moves that isn't castling by 20 points.
                if (kingSq == move.from() &&
                    move.typeOf() != chess::Move::CASTLING)
                    score = min ? 20 : -20;

                board.makeMove(move);

                if (depth == 0) {
                    score += evaluate(board);
                } else {
                    chess::Movelist child;
                    chess::movegen::legalmoves(child, board);

                    score += eval_tree(child, board, depth - 1, alpha, beta, control, !min).first;

                    if (score > 30000)
                        score--;
                    else if (score < -30000)
                        score++;
                }

                move.setScore(score);
                board.unmakeMove(move);

                best = std::min(best, score);
                beta = std::min(beta, best);

                if (beta <= alpha)
                    break; //AB-pruning cutoff
            }
        } else {
            std::int16_t best = std::numeric_limits<std::int16_t>::min();

            for (auto& move : ml) {
                std::int16_t score = 0;

                // disfavor making king moves that isn't castling by 20 points.
                if (kingSq == move.from() &&
                    move.typeOf() != chess::Move::CASTLING)
                    score = min ? 20 : -20;

                board.makeMove(move);

                if (depth == 0) {
                    score += evaluate(board);
                } else {
                    chess::Movelist child;
                    chess::movegen::legalmoves(child, board);

                    score += eval_tree(child, board, depth - 1, alpha, beta, control, !min).first;

                    if (score > 30000)
                        score--;
                    else if (score < -30000)
                        score++;
                }

                move.setScore(score);
                board.unmakeMove(move);

                best = std::max(best, score);
                alpha = std::max(alpha, best);

                if (alpha >= beta)
                    break; // AB-pruning cutoff
            }
        }

    finish:
        auto iter = min ?
            std::ranges::min_element(ml, {}, [](const chess::Move& m) { return m.score(); }) :
            std::ranges::max_element(ml, {}, [](const chess::Move& m) { return m.score(); });

        return {iter->score(), *iter};
    }

    virtual void run() {
        using namespace std::chrono_literals;

        std::mutex mine;
        while (!should_stop_.stop_requested()) {
            if (!searching) {
                std::unique_lock lk_(mine);
                worker_to_main.wait(lk_, [this]{return searching;});
            }

            SearchControl control {
                this,
                gameBoard.sideToMove() == chess::Color::WHITE ? go_flags_.wtime : go_flags_.btime,
                gameBoard.sideToMove() == chess::Color::WHITE ? go_flags_.winc : go_flags_.binc
            };

            control.atime = control.ourinc + control.ourtime / (5 + std::min(0u, 250 - gameBoard.fullMoveNumber()));

            start_time = std::chrono::steady_clock::now();

            control.us = gameBoard.sideToMove();
            control.them = ~control.us;

            chess::Movelist ml;

            chess::movegen::legalmoves(ml, gameBoard);

            std::uint8_t MAX_DEPTH = 1;

            if (control.atime > 1000)
                MAX_DEPTH = 3;

            if (control.atime > 5000)
                MAX_DEPTH++;

            if (control.atime > 10000)
                MAX_DEPTH++;

            auto [score, pv] = eval_tree(ml, gameBoard, MAX_DEPTH, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max(), control, control.us != chess::Color::WHITE);

            searching = false;
            should_stop = false;

            sendScore(score);
            sendDepth(MAX_DEPTH);
            sendResult(pv);
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

bool SearchControl::should_stop() const {
    if (engine->should_stop)
        return true;

    auto& flags = engine->go_flags_;

    if (flags.nodes < engine->ncount)
        return true;

    if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - engine->start_time).count() >= atime)
        return true;

    return false;
}

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

    return 0;
}