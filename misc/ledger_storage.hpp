/*
Copyright 2018 Ilja Honkonen
*/

#ifndef LEDGER_STORAGE_HPP
#define LEDGER_STORAGE_HPP

#include "accounts.hpp"
#include "transactions.hpp"

#include <boost/filesystem.hpp>
#include <cryptopp/blake2.h>

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>


namespace taraxa {


//! Usually Path == boost::filesystem::path
template<class Path> Path get_vote_path(
	const std::string& hash_hex,
	const Path& votes_path
) {
	using std::to_string;

	const auto nr_hash_chars = 2 * CryptoPP::BLAKE2s::DIGESTSIZE;
	if (hash_hex.size() != nr_hash_chars) {
		throw std::invalid_argument(
			"Hex format hash of vote must be " + to_string(nr_hash_chars)
			+ " characters but is " + to_string(hash_hex.size())
		);
	}

	auto vote_path = votes_path;
	vote_path /= hash_hex.substr(0, 2);
	vote_path /= hash_hex.substr(2, 2);
	vote_path /= hash_hex.substr(4);

	return vote_path;
}


//! Usually Path == boost::filesystem::path
template<class Path> Path get_transaction_path(
	const std::string& hash_hex,
	const Path& transactions_path
) {
	using std::to_string;

	const auto nr_hash_chars = 2 * CryptoPP::BLAKE2s::DIGESTSIZE;
	if (hash_hex.size() != nr_hash_chars) {
		throw std::invalid_argument(
			"Hex format hash of transaction must be " + to_string(nr_hash_chars)
			+ " characters but is " + to_string(hash_hex.size())
		);
	}

	auto transaction_path = transactions_path;
	transaction_path /= hash_hex.substr(0, 2);
	transaction_path /= hash_hex.substr(2, 2);
	transaction_path /= hash_hex.substr(4);

	return transaction_path;
}


//! Usually Path == boost::filesystem::path
template<class Path> Path get_account_path(
	const std::string& pubkey_hex,
	const Path& accounts_path
) {
	using std::to_string;

	const auto pubkey_hex_size = 2 * public_key_size(
		CryptoPP::ECDSA<CryptoPP::ECP, CryptoPP::SHA256>::PublicKey()
	);
	if (pubkey_hex.size() != pubkey_hex_size) {
		throw std::invalid_argument(
			"Hex format public key must be " + to_string(pubkey_hex_size)
			+ " characters but is " + to_string(pubkey_hex.size())
		);
	}

	auto account_path = accounts_path;
	account_path /= pubkey_hex.substr(0, 2);
	account_path /= pubkey_hex.substr(2);

	return account_path;
}


template<
	class Hasher
> std::tuple<
	std::map<std::string, Account<Hasher>>,
	std::map<std::string, Transaction<Hasher>>,
	std::map<std::string, Transient_Vote<Hasher>>
> load_ledger_data(
	const std::string& ledger_path_str,
	const bool verbose
) {
	using std::to_string;

	boost::filesystem::path ledger_path(ledger_path_str);
	if (ledger_path.size() > 0) {
		if (not boost::filesystem::exists(ledger_path)) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") "
				"Ledger directory doesn't exist: " + ledger_path_str
			);
		}
		if (not boost::filesystem::is_directory(ledger_path)) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") "
				"Ledger path isn't a directory: " + ledger_path_str
			);
		}
	}

	// load account data
	std::map<std::string, Account<Hasher>> accounts;
	auto accounts_path = ledger_path;
	accounts_path /= "accounts";
	if (
		boost::filesystem::exists(accounts_path)
		and boost::filesystem::is_directory(accounts_path)
	) {
		if (verbose) {
			std::cout << "Loading accounts from " << accounts_path << std::endl;
		}

		for (const auto& account_path: boost::filesystem::recursive_directory_iterator(accounts_path)) {
			if (boost::filesystem::is_directory(account_path)) {
				continue;
			}

			Account<Hasher> account;
			try {
				account.load(account_path.path().string(), verbose);
			} catch (...) {
				throw;
			}
			accounts[account.address_hex] = account;
		}
	}
	if (verbose) {
		std::cout << "Loaded " << accounts.size() << " accounts" << std::endl;
	}

	// load transaction data
	std::map<std::string, Transaction<Hasher>> transactions;
	auto transactions_path = ledger_path;
	transactions_path /= "transactions";
	if (boost::filesystem::exists(transactions_path)) {
		if (verbose) {
			std::cout << "Loading transactions from " << transactions_path << std::endl;
		}

		for (const auto& transaction_path: boost::filesystem::recursive_directory_iterator(transactions_path)) {
			if (boost::filesystem::is_directory(transaction_path)) {
				continue;
			}

			Transaction<Hasher> transaction;
			try {
				transaction.load(transaction_path.path().string(), verbose);
			} catch (...) {
				throw;
			}
			transactions[transaction.hash_hex] = transaction;
		}
	}
	if (verbose) {
		std::cout << "Loaded " << transactions.size() << " transactions" << std::endl;
	}

	// load vote data
	std::map<std::string, Transient_Vote<Hasher>> votes;
	auto votes_path = ledger_path;
	votes_path /= "votes";
	if (boost::filesystem::exists(votes_path)) {
		if (verbose) {
			std::cout << "Loading votes from " << votes_path << std::endl;
		}

		for (const auto& vote_path: boost::filesystem::recursive_directory_iterator(votes_path)) {
			if (boost::filesystem::is_directory(vote_path)) {
				continue;
			}

			Transient_Vote<Hasher> vote;
			try {
				vote.load(vote_path.path().string(), verbose);
			} catch (...) {
				throw;
			}
			votes[vote.hash_hex] = vote;
		}
	}
	if (verbose) {
		std::cout << "Loaded " << votes.size() << " votes" << std::endl;
	}

	return std::make_tuple(accounts, transactions, votes);
}


template<
	template<typename> class Transaction,
	class Hasher
> std::string add_transaction(
	const Transaction<Hasher>& transaction,
	const boost::filesystem::path& transactions_path,
	const boost::filesystem::path& accounts_path,
	const bool verbose
) {
	using std::to_string;

	const auto
		transaction_path = get_transaction_path(transaction.hash_hex, transactions_path),
		transaction_dir = transaction_path.parent_path();

	// make sure previous transaction exists and doesn't already have a next one
	if (transaction.previous_hex != "0000000000000000000000000000000000000000000000000000000000000000") {
		const auto previous_path = get_transaction_path(transaction.previous_hex, transactions_path);
		if (not boost::filesystem::exists(previous_path)) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") Previous transaction "
				+ previous_path.string() + " doesn't exist."
			);
		}

		Transaction<Hasher> previous_transaction;
		try {
			previous_transaction.load(previous_path.string(), verbose);
		} catch (const std::exception& e) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") "
				+ "Couldn't load previous transaction from "
				+ previous_path.string() + ": " + e.what()
			);
		}

		if (
			previous_transaction.next_hex.size() > 0
			and previous_transaction.next_hex != transaction.hash_hex
		) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") "
				+ "Previous transaction already has different next transaction"
			);
		}

		if (previous_transaction.next_hex.size() == 0) {
			// add current one as next of previous to make seeking easier
			if (verbose) {
				std::cout << "Appending transaction hash to previous transaction at "
					<< previous_path << std::endl;
			}
			// TODO if receive, only add if matching send exists
			previous_transaction.next_hex = transaction.hash_hex;
			previous_transaction.to_json_file(previous_path.string());
		}

	// check for different genesis transaction
	} else {
		const auto
			account_info_path = get_account_path(transaction.pubkey_hex, accounts_path),
			account_info_dir = account_info_path.parent_path();

		if (boost::filesystem::exists(account_info_path)) {
			Account<Hasher> existing_account;
			existing_account.load(account_info_path.string(), verbose);
			if (existing_account.genesis_transaction_hex != transaction.hash_hex) {
				throw std::invalid_argument(
					__FILE__ "(" + to_string(__LINE__) + ") "
					+ "Different genesis transaction already exists for account "
					+ transaction.pubkey_hex
				);
			} else {
				if (verbose) {
					std::cout << "Genesis transaction already exists." << std::endl;
				}
				// update modification time to make test makefiles simpler
				if (boost::filesystem::exists(transaction_path)) {
					boost::filesystem::last_write_time(transaction_path, std::time(nullptr));
				}
				return "";
			}
		}

		if (not boost::filesystem::exists(account_info_dir)) {
			if (verbose) {
				std::cout << "Account directory doesn't exist, creating..." << std::endl;
			}
			boost::filesystem::create_directories(account_info_dir);
		}

		if (verbose) {
			std::cout << "Writing account info to " << account_info_path << std::endl;
		}

		Account<Hasher> account;
		account.pubkey_hex = transaction.pubkey_hex;
		account.genesis_transaction_hex = transaction.hash_hex;
		account.balance_hex = "0000000000000000";
		account.to_json_file(account_info_path.string());
	}

	// in case of receive, add it to sending transaction for easier seeking
	if (transaction.send_hex.size() > 0) {
		const auto send_path = get_transaction_path(transaction.send_hex, transactions_path);
		if (not boost::filesystem::exists(send_path)) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") Matching send " + transaction.send_hex
				+ " for receive " + transaction.hash_hex + " doesn't exist"
			);
		}

		Transaction<Hasher> send_transaction;
		try {
			send_transaction.load(send_path.string(), verbose);
		} catch (const std::exception& e) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") "
				"Couldn't load send transaction from "
				+ send_path.string() + ": " + e.what()
			);
		}

		if (
			send_transaction.receive_hex.size() > 0
			and send_transaction.receive_hex != transaction.hash_hex
		) {
			throw std::invalid_argument(
				__FILE__ "(" + to_string(__LINE__) + ") "
				"Send transaction " + send_transaction.hash_hex
				+ " already has a different receive " + send_transaction.receive_hex
				+ " instead of " + transaction.hash_hex
			);
		}

		if (send_transaction.receive_hex.size() == 0) {
			if (verbose) {
				std::cout << "Appending receive hash to send at "
					<< send_path << std::endl;
			}
			send_transaction.receive_hex = transaction.hash_hex;
			send_transaction.to_json_file(send_path.string());
		}
	}

	/*
	Add transaction given on stdin to ledger data
	*/
	if (boost::filesystem::exists(transaction_path)) {
		if (verbose) {
			std::cout << "Transaction already exists." << std::endl;
		}
		// update modification time to make test makefiles simpler
		if (boost::filesystem::exists(transaction_path)) {
			boost::filesystem::last_write_time(transaction_path, std::time(nullptr));
		}
		return "";
	}
	if (not boost::filesystem::exists(transaction_dir)) {
		if (verbose) {
			std::cout << "Transaction directory doesn't exist, creating..." << std::endl;
		}
		boost::filesystem::create_directories(transaction_dir);
	}
	if (verbose) {
		std::cout << "Writing transaction to " << transaction_path << std::endl;
	}

	transaction.to_json_file(transaction_path.string());

	return transaction_path.string();
}


} // namespace taraxa

#endif // ifndef LEDGER_STORAGE_HPP