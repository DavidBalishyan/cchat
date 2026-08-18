#!/usr/bin/env escript
%%! -noshell
-mode(compile).

-define(COLOR_RESET, "\033[0m").
-define(COLOR_RED, "\033[31m").
-define(COLOR_GREEN, "\033[32m").
-define(COLOR_YELLOW, "\033[33m").
-define(COLOR_CYAN, "\033[36m").
-define(PROTOCOL_VERSION, "CCHAT/1").
-define(MAX_USERNAME_LENGTH, 24).
-define(MAX_PASSWORD_LENGTH, 128).
-define(AUTHENTICATION_TIMEOUT, 15000).

main([ServerAddress, Username]) ->
    case {parse_address(ServerAddress), valid_username(Username)} of
        {{ok, Host, Port}, true} ->
            read_password_and_connect(ServerAddress, Host, Port, Username);
        {error, _} ->
            print_error("Invalid address format. Use <host:port>"),
            ok;
        {_, false} ->
            print_error(
                "Invalid username. Use 1-24 letters, numbers, '_' or '-'"
            ),
            ok
    end;
main(_) ->
    print_error("Usage: CLIENT <host:port> <username>"),
    ok.

parse_address(Address) ->
    Pattern = "^([a-zA-Z0-9.\\-]+):([0-9]{1,5})$",
    case re:run(Address, Pattern, [{capture, [1, 2], list}]) of
        {match, [Host, PortText]} ->
            {ok, Host, list_to_integer(PortText)};
        nomatch ->
            error
    end.

valid_username(Username) ->
    Pattern = "^[a-zA-Z0-9_-]{1," ++ integer_to_list(?MAX_USERNAME_LENGTH) ++ "}$",
    re:run(Username, Pattern, [{capture, none}]) =:= match.

read_password_and_connect(ServerAddress, Host, Port, Username) ->
    io:put_chars("Password: "),
    case read_password() of
        {ok, Password} ->
            PasswordBinary = unicode:characters_to_binary(Password),
            case byte_size(PasswordBinary) of
                0 ->
                    print_error("Password cannot be empty"),
                    ok;
                Length when Length =< ?MAX_PASSWORD_LENGTH ->
                    connect(ServerAddress, Host, Port, Username, PasswordBinary);
                _ ->
                    print_error("Password is too long"),
                    ok
            end;
        eof ->
            ok;
        {error, Reason} ->
            print_error(["Unable to read password: ", format_reason(Reason)]),
            ok
    end.

read_password() ->
    case io:get_password() of
        Password when is_list(Password) ->
            {ok, Password};
        {error, enotsup} ->
            read_visible_password();
        eof ->
            eof;
        {error, Reason} ->
            {error, Reason}
    end.

read_visible_password() ->
    case io:get_line("") of
        Line when is_list(Line) ->
            {ok, string:trim(Line, trailing, "\r\n")};
        eof ->
            eof;
        {error, Reason} ->
            {error, Reason}
    end.

connect(ServerAddress, Host, Port, Username, Password) when Port =< 65535 ->
    Options = [binary, {packet, line}, {active, false}],
    case gen_tcp:connect(Host, Port, Options) of
        {ok, Socket} ->
            authenticate(Socket, ServerAddress, Username, Password);
        {error, Reason} ->
            print_error(["Failed to connect: ", format_reason(Reason)]),
            ok
    end;
connect(_ServerAddress, _Host, _Port, _Username, _Password) ->
    print_error("Failed to connect: invalid port"),
    ok.

authenticate(Socket, ServerAddress, Username, Password) ->
    Handshake = [
        ?PROTOCOL_VERSION,
        "\n",
        Username,
        "\n",
        Password,
        "\n"
    ],
    case gen_tcp:send(Socket, Handshake) of
        ok ->
            receive_authentication_result(Socket, ServerAddress, Username);
        {error, Reason} ->
            print_error(["Authentication failed: ", format_reason(Reason)]),
            gen_tcp:close(Socket),
            ok
    end.

receive_authentication_result(Socket, ServerAddress, Username) ->
    case gen_tcp:recv(Socket, 0, ?AUTHENTICATION_TIMEOUT) of
        {ok, <<"OK\n">>} ->
            run(Socket, ServerAddress, Username);
        {ok, <<"OK\r\n">>} ->
            run(Socket, ServerAddress, Username);
        {ok, Response} ->
            Reason = authentication_error(Response),
            print_error(["Authentication failed: ", Reason]),
            gen_tcp:close(Socket),
            ok;
        {error, Reason} ->
            print_error(["Authentication failed: ", format_reason(Reason)]),
            gen_tcp:close(Socket),
            ok
    end.

authentication_error(Response) ->
    Line = strip_line_ending(Response),
    case Line of
        <<"ERROR ", Reason/binary>> -> Reason;
        _ -> "unexpected server response"
    end.

run(Socket, ServerAddress, Username) ->
    io:put_chars([
        ?COLOR_YELLOW,
        "Connected to ",
        ServerAddress,
        " as ",
        Username,
        ?COLOR_RESET,
        "\n",
        ?COLOR_YELLOW,
        "Type your message and press Enter. Use /quit to exit.",
        ?COLOR_RESET,
        "\n"
    ]),
    Receiver = spawn(fun() -> receive_messages(Socket) end),
    input_loop(Socket, Receiver).

input_loop(Socket, Receiver) ->
    print_prompt(),
    case io:get_line("") of
        eof ->
            close(Socket, Receiver);
        {error, _Reason} ->
            close(Socket, Receiver);
        Line ->
            handle_input(string:trim(Line), Socket, Receiver)
    end.

handle_input([], Socket, Receiver) ->
    input_loop(Socket, Receiver);
handle_input("/quit", Socket, Receiver) ->
    close(Socket, Receiver);
handle_input(Text, Socket, Receiver) ->
    Message = unicode:characters_to_binary(Text),
    case gen_tcp:send(Socket, [Message, "\n"]) of
        ok ->
            input_loop(Socket, Receiver);
        {error, Reason} ->
            print_error(["Error sending message: ", format_reason(Reason)]),
            close(Socket, Receiver)
    end.

receive_messages(Socket) ->
    case gen_tcp:recv(Socket, 0) of
        {ok, Data} ->
            io:put_chars([
                "\r",
                ?COLOR_CYAN,
                strip_line_ending(Data),
                ?COLOR_RESET,
                "\n"
            ]),
            print_prompt(),
            receive_messages(Socket);
        {error, closed} ->
            io:put_chars([?COLOR_RED, "Server disconnected.", ?COLOR_RESET, "\n"]),
            erlang:halt(0);
        {error, Reason} ->
            print_error(["Connection error: ", format_reason(Reason)]),
            erlang:halt(0)
    end.

strip_line_ending(Data) ->
    WithoutNewline = strip_final_byte(Data, $\n),
    strip_final_byte(WithoutNewline, $\r).

strip_final_byte(<<>>, _Byte) ->
    <<>>;
strip_final_byte(Data, Byte) ->
    Size = byte_size(Data),
    case binary:at(Data, Size - 1) of
        Byte -> binary:part(Data, 0, Size - 1);
        _ -> Data
    end.

close(Socket, Receiver) ->
    exit(Receiver, kill),
    gen_tcp:close(Socket),
    ok.

print_prompt() ->
    io:put_chars([?COLOR_GREEN, "You: ", ?COLOR_RESET]).

print_error(Message) ->
    io:put_chars([?COLOR_RED, Message, ?COLOR_RESET, "\n"]).

format_reason(Reason) when is_atom(Reason) ->
    inet:format_error(Reason);
format_reason(Reason) ->
    io_lib:format("~p", [Reason]).
