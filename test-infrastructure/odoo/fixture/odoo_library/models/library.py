# Controlled Odoo fixture for codebase-memory-mcp Odoo-aware tests.
# Every cross-reference here has a KNOWN expected graph edge — see
# test-infrastructure/odoo/verify_odoo.py for the assertions.
from odoo import models, fields, api, _


class LibraryBook(models.Model):
    _name = 'library.book'
    _description = 'Library Book'

    name = fields.Char(required=True)
    member_id = fields.Many2one('library.member', string='Borrower')
    page_count = fields.Integer()
    state = fields.Selection([('avail', 'Available'), ('out', 'On Loan')], default='avail')
    summary = fields.Char(compute='_compute_summary', store=True)

    @api.depends('name', 'page_count')
    def _compute_summary(self):
        for book in self:
            book.summary = self._format_summary(book.name, book.page_count)

    def _format_summary(self, name, pages):
        return "%s (%s pages)" % (name, pages)

    def borrow_for(self, partner):
        # ORM call: must resolve to library.member model, not a bare 'create'
        member = self.env['library.member'].create({'partner_id': partner.id})
        self.write({'member_id': member.id, 'state': 'out'})
        self._notify_borrowed(member)
        return member

    def _notify_borrowed(self, member):
        member.send_notice()


class LibraryMember(models.Model):
    _name = 'library.member'
    _description = 'Library Member'

    partner_id = fields.Many2one('res.partner', required=True)
    book_ids = fields.One2many('library.book', 'member_id')

    def send_notice(self):
        return True

    @api.model_create_multi
    def create(self, vals_list):
        # ORM override on this model
        return super().create(vals_list)


class LibraryBookArchive(models.Model):
    # Prototype inheritance: a NEW model that inherits from library.book
    # → expect INHERITS_MODEL: library.book.archive -> library.book
    _name = 'library.book.archive'
    _inherit = 'library.book'

    archived_on = fields.Date()


class ResPartnerLibrary(models.Model):
    # Extension inheritance: extends an external core model (no own _name)
    # → expect DEFINES_MODEL: ResPartnerLibrary -> res.partner
    _inherit = 'res.partner'

    loan_count = fields.Integer(compute='_compute_loan_count')

    def _compute_loan_count(self):
        for partner in self:
            partner.loan_count = self.env['library.book'].search_count(
                [('member_id.partner_id', '=', partner.id)]
            )

    def reset_loans(self):
        # calls a method defined on the SAME inherited model
        self._compute_loan_count()
