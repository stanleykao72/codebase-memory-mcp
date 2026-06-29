# Controlled fixture controller — known expected edges.
from odoo import http
from odoo.http import request


class LibraryController(http.Controller):

    @http.route('/library/books', type='json', auth='user', methods=['GET'])
    def list_books(self, **kw):
        # ORM call via request.env — must resolve to library.book
        books = request.env['library.book'].search([('state', '=', 'avail')])
        return books.read(['name', 'summary'])

    @http.route('/library/borrow', type='json', auth='user', methods=['POST'])
    def borrow(self, book_id, partner_id, **kw):
        book = request.env['library.book'].browse(book_id)
        partner = request.env['res.partner'].browse(partner_id)
        # cross-object call: must resolve to LibraryBook.borrow_for
        return book.borrow_for(partner)
